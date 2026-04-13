/** @file
 *  @brief Short-lived privileged worker that executes fixed CRIU commands.
 *
 *  @details
 *  The worker never accepts raw user-supplied image paths or free-form CRIU
 *  argv. It receives fully-resolved broker-owned paths from the daemon and
 *  translates them into one of a small number of fixed dump/restore commands.
 */

#include "src/csrc/worker.h"

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "src/csrc/util.h"

namespace snapshotd {

namespace fs = std::filesystem;

namespace {

constexpr auto kNamespaceRestoreTimeout = std::chrono::seconds(20);
constexpr auto kHostRestoreTerminationGrace = std::chrono::milliseconds(500);
volatile sig_atomic_t g_host_restore_child_pgid = -1;
volatile sig_atomic_t g_host_restore_termination_signal = 0;

/** @brief Make stdin/stdout/stderr point at a usable controlling terminal. */
void PrepareControllingTty(int client_tty_fd);

/** @brief Return true when one CRIU argv vector already contains a given option. */
bool ContainsArg(const std::vector<std::string>& args, const std::string& token) {
  for (const std::string& arg : args) {
    if (arg == token || arg.rfind(token + "=", 0) == 0) {
      return true;
    }
  }
  return false;
}

/** @brief Append one diagnostic line to a worker-owned log file. */
void AppendLogLine(const fs::path& path, const std::string& line) {
  EnsureDir(path.parent_path(), 0700);
  std::ofstream output(path, std::ios::out | std::ios::app);
  if (!output.is_open()) {
    throw std::runtime_error("failed to open log for append: " + path.string());
  }
  output << line << "\n";
}

/** @brief Validate worker config before any privileged CRIU invocation. */
void ValidateWorkerConfig(const WorkerConfig& config) {
  if (config.operation != "dump" && config.operation != "restore") {
    throw std::runtime_error("unsupported worker operation: " + config.operation);
  }
  if (!IsAbsolutePath(config.criu_bin)) {
    throw std::runtime_error("criu path must be absolute");
  }
  if ((config.namespace_dump || config.namespace_restore) &&
      !IsAbsolutePath(config.criu_ns_bin)) {
    throw std::runtime_error("criu-ns path must be absolute when namespace mode is enabled");
  }
  if (!config.state_dir.is_absolute() || !config.job_dir.is_absolute() ||
      !config.checkpoint_dir.is_absolute()) {
    throw std::runtime_error("worker paths must be absolute");
  }
  // Recreate the expected directory layout before touching CRIU so the worker
  // never follows caller-controlled relative paths.
  EnsureDir(config.state_dir, 0755);
  EnsureDir(config.job_dir, 0700);
  EnsureDir(config.checkpoint_dir, 0700);
  if (!IsPathBeneath(config.state_dir, config.job_dir)) {
    throw std::runtime_error("job dir is outside state dir");
  }
  if (!IsPathBeneath(config.job_dir, config.checkpoint_dir)) {
    throw std::runtime_error("checkpoint dir is outside job dir");
  }
  if (config.operation == "dump" && config.pid <= 1) {
    throw std::runtime_error("dump requires a valid positive pid");
  }
}

/** @brief Convert a command vector into exec-ready `argv`. */
std::vector<char*> MakeArgv(const std::vector<std::string>& command) {
  std::vector<char*> argv;
  argv.reserve(command.size() + 1);
  for (const std::string& token : command) {
    argv.push_back(const_cast<char*>(token.c_str()));
  }
  argv.push_back(nullptr);
  return argv;
}

/** @brief Replace the current process with a scrubbed CRIU command. */
[[noreturn]] void ExecCommand(const std::vector<std::string>& command) {
  // Privileged CRIU invocations run with a scrubbed environment so ambient
  // variables such as CRIU_CONFIG_FILE cannot steer the worker unexpectedly.
  if (clearenv() != 0) {
    _exit(127);
  }
  setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
  setenv("LANG", "C", 1);
  std::vector<char*> argv = MakeArgv(command);
  char* const envp[] = {
      const_cast<char*>("PATH=/usr/sbin:/usr/bin:/sbin:/bin"),
      const_cast<char*>("LANG=C"),
      nullptr,
  };
  execve(command.front().c_str(), argv.data(), envp);
  _exit(127);
}

/** @brief Fork, exec one command, and require a successful exit status. */
int ForkExec(const std::vector<std::string>& command) {
  const pid_t child = fork();
  if (child < 0) {
    ThrowErrno("fork");
  }
  if (child == 0) {
    ExecCommand(command);
  }

  int status = 0;
  if (waitpid(child, &status, 0) < 0) {
    ThrowErrno("waitpid");
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw std::runtime_error("worker child command failed: " + JoinCommandLine(command));
  }
  return 0;
}

/** @brief Record the signal that should be forwarded to a host-restore subtree. */
void RequestHostRestoreTermination(int signal_number) {
  g_host_restore_termination_signal = signal_number;
}

/**
 * @brief Run host restore with full tty handoff and signal forwarding.
 *
 * The child becomes a session leader, claims the caller tty when possible, and
 * the parent makes sure timeout or interrupt signals terminate the whole
 * restore process group rather than only the wrapper process.
 */
int ForkExecRestoreWithControllingTty(
    const std::vector<std::string>& command,
    int client_tty_fd) {
  const pid_t child = fork();
  if (child < 0) {
    ThrowErrno("fork");
  }
  if (child == 0) {
    try {
      if (setsid() < 0) {
        ThrowErrno("setsid");
      }
      PrepareControllingTty(client_tty_fd);
      ExecCommand(command);
    } catch (...) {
      _exit(127);
    }
  }

  if (client_tty_fd > STDERR_FILENO) {
    close(client_tty_fd);
  }

  struct sigaction forward_action {};
  sigemptyset(&forward_action.sa_mask);
  forward_action.sa_handler = RequestHostRestoreTermination;

  struct sigaction old_term {};
  struct sigaction old_int {};
  struct sigaction old_hup {};
  g_host_restore_child_pgid = child;
  g_host_restore_termination_signal = 0;
  if (sigaction(SIGTERM, &forward_action, &old_term) != 0 ||
      sigaction(SIGINT, &forward_action, &old_int) != 0 ||
      sigaction(SIGHUP, &forward_action, &old_hup) != 0) {
    g_host_restore_child_pgid = -1;
    ThrowErrno("sigaction");
  }

  int status = 0;
  bool forwarded_termination = false;
  auto termination_deadline = std::chrono::steady_clock::time_point::max();
  while (true) {
    const int wait_flags =
        (g_host_restore_termination_signal != 0 || forwarded_termination) ? WNOHANG : 0;
    const pid_t waited = waitpid(child, &status, wait_flags);
    if (waited == child) {
      break;
    }
    if (waited == 0) {
      if (!forwarded_termination) {
        const int signal_number =
            g_host_restore_termination_signal != 0 ? g_host_restore_termination_signal : SIGTERM;
        (void)!kill(-child, signal_number);
        forwarded_termination = true;
        termination_deadline = std::chrono::steady_clock::now() + kHostRestoreTerminationGrace;
      } else if (std::chrono::steady_clock::now() >= termination_deadline) {
        (void)!kill(-child, SIGKILL);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    if (waited < 0 && errno == EINTR) {
      if (g_host_restore_termination_signal != 0 && !forwarded_termination) {
        (void)!kill(-child, g_host_restore_termination_signal);
        forwarded_termination = true;
        termination_deadline = std::chrono::steady_clock::now() + kHostRestoreTerminationGrace;
      }
      continue;
    }
    if (waited < 0) {
      const int saved_errno = errno;
      g_host_restore_child_pgid = -1;
      g_host_restore_termination_signal = 0;
      (void)!sigaction(SIGTERM, &old_term, nullptr);
      (void)!sigaction(SIGINT, &old_int, nullptr);
      (void)!sigaction(SIGHUP, &old_hup, nullptr);
      errno = saved_errno;
      ThrowErrno("waitpid");
    }
  }

  g_host_restore_child_pgid = -1;
  const int termination_signal = g_host_restore_termination_signal;
  g_host_restore_termination_signal = 0;
  (void)!sigaction(SIGTERM, &old_term, nullptr);
  (void)!sigaction(SIGINT, &old_int, nullptr);
  (void)!sigaction(SIGHUP, &old_hup, nullptr);

  if (termination_signal != 0) {
    throw std::runtime_error(
        "worker child command interrupted by signal " + std::to_string(termination_signal));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw std::runtime_error("worker child command failed: " + JoinCommandLine(command));
  }
  return 0;
}

/** @brief Read the immediate child pid list exported by `/proc/<pid>/task/.../children`. */
std::vector<pid_t> ReadChildPids(pid_t pid) {
  const fs::path path = fs::path("/proc") / PidToString(pid) / "task" / PidToString(pid) /
                        "children";
  std::vector<pid_t> children;
  std::istringstream input(ReadTextFile(path));
  long long value = 0;
  while (input >> value) {
    if (value > 0) {
      children.push_back(static_cast<pid_t>(value));
    }
  }
  return children;
}

/** @brief Read the kernel `NSpid` chain for one process from `/proc/<pid>/status`. */
std::vector<pid_t> ReadNSpidValues(pid_t pid) {
  const fs::path path = fs::path("/proc") / PidToString(pid) / "status";
  std::istringstream input(ReadTextFile(path));
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("NSpid:", 0) != 0) {
      continue;
    }
    std::istringstream fields(line.substr(6));
    std::vector<pid_t> values;
    long long value = 0;
    while (fields >> value) {
      values.push_back(static_cast<pid_t>(value));
    }
    return values;
  }
  return {};
}

/** @brief Map a restored local pid in a pid namespace back to its host pid. */
pid_t ResolveHostPidFromLocalPid(pid_t ns_init_host_pid, pid_t local_pid) {
  const auto deadline = std::chrono::steady_clock::now() + kNamespaceRestoreTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    for (pid_t host_pid : ReadChildPids(ns_init_host_pid)) {
      // The namespace init process is the parent of the restored tree. Match on
      // the innermost NSpid value to recover the host pid for bookkeeping.
      const std::vector<pid_t> nspid_values = ReadNSpidValues(host_pid);
      if (!nspid_values.empty() && nspid_values.back() == local_pid) {
        return host_pid;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw std::runtime_error(
      "timed out resolving host pid for restored local pid " + PidToString(local_pid));
}

/** @brief Send a newline-delimited status payload across the namespace status pipe. */
void WriteStatusPayload(int fd, const std::map<std::string, std::string>& values) {
  const std::string payload = SerializeKeyValueMap(values);
  const char* data = payload.data();
  std::size_t remaining = payload.size();
  while (remaining > 0) {
    const ssize_t written = write(fd, data, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    data += written;
    remaining -= static_cast<std::size_t>(written);
  }
}

/** @brief Read one status payload from the pid-namespace restore helper pipe. */
std::map<std::string, std::string> ReadStatusPayload(int fd) {
  std::string payload;
  const auto deadline = std::chrono::steady_clock::now() + kNamespaceRestoreTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd poll_fd {};
    poll_fd.fd = fd;
    poll_fd.events = POLLIN;
    const int poll_result = poll(&poll_fd, 1, 50);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno("poll(status pipe)");
    }
    if (poll_result == 0) {
      continue;
    }
    char buffer[4096];
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno("read(status pipe)");
    }
    if (count == 0) {
      break;
    }
    payload.append(buffer, static_cast<std::size_t>(count));
    if (payload.find('\n') != std::string::npos) {
      break;
    }
  }
  if (payload.empty()) {
    throw std::runtime_error("timed out waiting for pid-namespace restore status");
  }
  return ParseKeyValueText(payload);
}

/** @brief Re-mount `/proc` inside the private namespace used for pidns restore. */
void MountNewProc() {
  if (mount(nullptr, "/", nullptr, MS_SLAVE | MS_REC, nullptr) != 0) {
    ThrowErrno("mount(/, MS_SLAVE|MS_REC)");
  }
  if (mount("proc", "/proc", "proc", 0, nullptr) != 0) {
    ThrowErrno("mount(/proc)");
  }
}

/**
 * @brief Prepare job-control state for restore.
 *
 * When the daemon passes a caller tty, the worker duplicates it to stdio,
 * claims it as the controlling terminal, and makes the restore process group
 * foreground when the kernel allows that transition.
 */
void PrepareControllingTty(int client_tty_fd) {
  auto set_foreground_process_group = [] {
    if (tcsetpgrp(STDIN_FILENO, getpgrp()) != 0) {
      ThrowErrno("tcsetpgrp");
    }
  };
  // If the client passed a TTY fd, dup2 it onto stdin/stdout/stderr so the
  // restored process inherits the caller's actual terminal.
  if (client_tty_fd >= 0 && isatty(client_tty_fd)) {
    for (int fd_num = STDIN_FILENO; fd_num <= STDERR_FILENO; ++fd_num) {
      if (dup2(client_tty_fd, fd_num) < 0) {
        ThrowErrno("dup2(client tty)");
      }
    }
    if (client_tty_fd > STDERR_FILENO) {
      close(client_tty_fd);
    }
    if (ioctl(STDIN_FILENO, TIOCSCTTY, 1) == 0) {
      set_foreground_process_group();
    } else if (errno != EPERM) {
      ThrowErrno("ioctl(TIOCSCTTY)");
    }
    return;
  }

  if (isatty(STDIN_FILENO)) {
    if (ioctl(STDIN_FILENO, TIOCSCTTY, 1) != 0) {
      ThrowErrno("ioctl(TIOCSCTTY)");
    }
    set_foreground_process_group();
    return;
  }
  int master_fd = -1;
  int slave_fd = -1;
  if (openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr) != 0) {
    ThrowErrno("openpty");
  }
  // CRIU expects a controlling TTY in several shell-job flows. When the worker
  // is started without one, fabricate a private PTY pair just for setup.
  if (dup2(slave_fd, STDIN_FILENO) < 0) {
    ThrowErrno("dup2(slave tty)");
  }
  if (ioctl(STDIN_FILENO, TIOCSCTTY, 1) != 0) {
    ThrowErrno("ioctl(TIOCSCTTY)");
  }
  set_foreground_process_group();
  if (slave_fd > STDERR_FILENO) {
    close(slave_fd);
  }
  if (master_fd >= 0) {
    const int flags = fcntl(master_fd, F_GETFD);
    if (flags >= 0) {
      (void)!fcntl(master_fd, F_SETFD, flags | FD_CLOEXEC);
    }
  }
}

/** @brief Find the first non-CRIU child under pid 1 after pidns restore completes. */
pid_t FindRestoredLocalPid(pid_t criu_pid) {
  for (pid_t child_pid : ReadChildPids(1)) {
    if (child_pid != criu_pid) {
      return child_pid;
    }
  }
  return 0;
}

/**
 * @brief Run inside pid 1 of the new namespace and report the restored pid out.
 *
 * This helper owns the namespace-local restore lifecycle and communicates the
 * selected restored local pid back to the host-side parent over @p status_fd.
 */
int RunNamespaceRestoreInit(const WorkerConfig& config, int status_fd, const fs::path& log_path) {
  bool status_sent = false;
  pid_t criu_pid = 0;
  try {
    AppendLogLine(log_path, "pidns init: starting");
    if (unshare(CLONE_NEWNS) != 0) {
      ThrowErrno("unshare(CLONE_NEWNS)");
    }
    if (setsid() < 0) {
      ThrowErrno("setsid");
    }
    PrepareControllingTty(config.tty_fd);
    MountNewProc();

    const fs::path images_dir = config.checkpoint_dir / "images";
    const fs::path work_dir = config.checkpoint_dir / "restore-work";
    std::vector<std::string> command = {
        config.criu_bin,
        "restore",
        "--no-default-config",
        "--images-dir",
        images_dir.string(),
        "--work-dir",
        work_dir.string(),
        "--log-file",
        log_path.string(),
        "--shell-job",
        "-d",
    };
    if (!ContainsArg(config.extra_args, "--ghost-limit")) {
      command.push_back("--ghost-limit=64M");
    }
    if (!ContainsArg(config.extra_args, "--link-remap")) {
      command.push_back("--link-remap");
    }
    command.insert(command.end(), config.extra_args.begin(), config.extra_args.end());

    criu_pid = fork();
    if (criu_pid < 0) {
      ThrowErrno("fork");
    }
    if (criu_pid == 0) {
      // The namespace init process becomes pid 1. It launches CRIU restore and
      // later reports the restored local pid back to the host-side parent.
      ExecCommand(command);
    }

    AppendLogLine(log_path, "pidns init: spawned criu pid " + PidToString(criu_pid));

    const auto deadline = std::chrono::steady_clock::now() + kNamespaceRestoreTimeout;
    pid_t restored_local_pid = 0;
    bool criu_exited = false;
    while (true) {
      if (criu_exited && restored_local_pid == 0) {
        restored_local_pid = FindRestoredLocalPid(criu_pid);
        if (restored_local_pid > 0) {
          AppendLogLine(
              log_path,
              "pidns init: selected restored local pid " + PidToString(restored_local_pid));
          WriteStatusPayload(
              status_fd, {{"restored_local_pid", PidToString(restored_local_pid)}});
          status_sent = true;
        }
      }

      int status = 0;
      const pid_t waited = waitpid(-1, &status, WNOHANG);
      if (waited < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno == ECHILD) {
          break;
        }
        ThrowErrno("waitpid");
      }
      if (waited == 0) {
        if (restored_local_pid == 0 &&
            std::chrono::steady_clock::now() >= deadline) {
          throw std::runtime_error("timed out detecting restored pid in pid namespace");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      if (waited == criu_pid) {
        if (!WIFEXITED(status)) {
          throw std::runtime_error("CRIU restore exited abnormally in pid namespace");
        }
        if (WEXITSTATUS(status) != 0) {
          throw std::runtime_error(
              "CRIU restore exited with code " + std::to_string(WEXITSTATUS(status)) +
              " in pid namespace");
        }
        criu_exited = true;
        AppendLogLine(log_path, "pidns init: criu exited with code 0");
      }
    }

    if (restored_local_pid == 0) {
      throw std::runtime_error("restored pid was never discovered in pid namespace");
    }
    return 0;
  } catch (const std::exception& error) {
    if (!status_sent) {
      WriteStatusPayload(status_fd, {{"error", error.what()}});
    }
    try {
      AppendLogLine(log_path, std::string("pid-namespace restore init failed: ") + error.what());
    } catch (...) {
    }
    return 1;
  }
}

/** @brief Orchestrate pid-namespace restore and translate the result back to host state. */
int RunNamespaceRestore(const WorkerConfig& config) {
  ValidateWorkerConfig(config);
  const fs::path images_dir = config.checkpoint_dir / "images";
  const fs::path work_dir = config.checkpoint_dir / "restore-work";
  const fs::path log_path = config.checkpoint_dir / "logs" / "restore.log";
  const fs::path pidfile = config.checkpoint_dir / "restore.pid";
  EnsureDir(images_dir, 0700);
  EnsureDir(work_dir, 0700);
  EnsureDir(log_path.parent_path(), 0700);
  int status_pipe[2] = {-1, -1};
  if (pipe2(status_pipe, O_CLOEXEC) != 0) {
    ThrowErrno("pipe2");
  }

  if (unshare(CLONE_NEWPID) != 0) {
    close(status_pipe[0]);
    close(status_pipe[1]);
    ThrowErrno("unshare(CLONE_NEWPID)");
  }

  const pid_t ns_init_host_pid = fork();
  if (ns_init_host_pid < 0) {
    close(status_pipe[0]);
    close(status_pipe[1]);
    ThrowErrno("fork");
  }
  if (ns_init_host_pid == 0) {
    close(status_pipe[0]);
    const int exit_code = RunNamespaceRestoreInit(config, status_pipe[1], log_path);
    close(status_pipe[1]);
    _exit(exit_code);
  }

  close(status_pipe[1]);
  // The parent stays in the host namespace and translates the restored local
  // pid back into a host pid for the daemon's persistent metadata.
  const std::map<std::string, std::string> payload = ReadStatusPayload(status_pipe[0]);
  close(status_pipe[0]);
  const auto error_it = payload.find("error");
  if (error_it != payload.end()) {
    throw std::runtime_error(error_it->second);
  }
  const auto local_pid_it = payload.find("restored_local_pid");
  if (local_pid_it == payload.end()) {
    throw std::runtime_error("pid-namespace restore did not return a restored pid");
  }
  const pid_t restored_local_pid = static_cast<pid_t>(std::stol(local_pid_it->second));
  const pid_t restored_host_pid = ResolveHostPidFromLocalPid(ns_init_host_pid, restored_local_pid);
  WriteTextFile(pidfile, PidToString(restored_host_pid) + "\n", 0600, 0700);
  return 0;
}

/** @brief Parse the daemon-owned worker argv into a validated WorkerConfig. */
WorkerConfig ParseArgs(int argc, char** argv) {
  WorkerConfig config;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    auto require_value = [&](const std::string& flag) -> std::string {
      if (index + 1 >= argc) {
        throw std::runtime_error("missing value for " + flag);
      }
      ++index;
      return argv[index];
    };
    if (arg == "--operation") {
      config.operation = require_value(arg);
    } else if (arg == "--state-dir") {
      config.state_dir = require_value(arg);
    } else if (arg == "--job-dir") {
      config.job_dir = require_value(arg);
    } else if (arg == "--checkpoint-dir") {
      config.checkpoint_dir = require_value(arg);
    } else if (arg == "--criu-bin") {
      config.criu_bin = require_value(arg);
    } else if (arg == "--criu-ns-bin") {
      config.criu_ns_bin = require_value(arg);
    } else if (arg == "--pid") {
      config.pid = static_cast<pid_t>(std::stol(require_value(arg)));
    } else if (arg == "--namespace-dump") {
      config.namespace_dump = true;
    } else if (arg == "--namespace-restore") {
      config.namespace_restore = true;
    } else if (arg == "--tty-fd") {
      config.tty_fd = static_cast<int>(std::stol(require_value(arg)));
    } else if (arg == "--extra-arg") {
      config.extra_args.push_back(require_value(arg));
    } else {
      throw std::runtime_error("unknown worker flag: " + arg);
    }
  }
  return config;
}

}  // namespace

std::vector<std::string> BuildDumpCommand(const WorkerConfig& config) {
  ValidateWorkerConfig(config);
  const fs::path images_dir = config.checkpoint_dir / "images";
  const fs::path work_dir = config.checkpoint_dir / "work";
  const fs::path log_path = config.checkpoint_dir / "logs" / "dump.log";
  EnsureDir(images_dir, 0700);
  EnsureDir(work_dir, 0700);
  EnsureDir(log_path.parent_path(), 0700);
  std::vector<std::string> command;
  if (config.namespace_dump) {
    command = {
        config.criu_ns_bin,
        "dump",
        "--criu-binary",
        config.criu_bin,
    };
  } else {
    command = {
        config.criu_bin,
        "dump",
    };
  }
  command.insert(
      command.end(),
      {
          "--no-default-config",
          "-t",
          PidToString(config.pid),
          "--images-dir",
          images_dir.string(),
          "--work-dir",
          work_dir.string(),
          "--log-file",
          log_path.string(),
          "--leave-running",
          "--shell-job",
      });
  if (!ContainsArg(config.extra_args, "--ghost-limit")) {
    command.push_back("--ghost-limit=64M");
  }
  if (!ContainsArg(config.extra_args, "--link-remap")) {
    command.push_back("--link-remap");
  }
  command.insert(command.end(), config.extra_args.begin(), config.extra_args.end());
  return command;
}

std::vector<std::string> BuildRestoreCommand(const WorkerConfig& config) {
  ValidateWorkerConfig(config);
  const fs::path images_dir = config.checkpoint_dir / "images";
  const fs::path work_dir = config.checkpoint_dir / "restore-work";
  const fs::path log_path = config.checkpoint_dir / "logs" / "restore.log";
  const fs::path pidfile = config.checkpoint_dir / "restore.pid";
  EnsureDir(images_dir, 0700);
  EnsureDir(work_dir, 0700);
  EnsureDir(log_path.parent_path(), 0700);
  std::vector<std::string> command;
  if (!config.namespace_restore) {
    command = {
        config.criu_bin,
        "restore",
    };
  }
  command.insert(
      command.end(),
      {
          "--no-default-config",
          "--images-dir",
          images_dir.string(),
          "--work-dir",
          work_dir.string(),
          "--log-file",
          log_path.string(),
          "--pidfile",
          pidfile.string(),
          "--shell-job",
          "-d",
      });
  if (!ContainsArg(config.extra_args, "--ghost-limit")) {
    command.push_back("--ghost-limit=64M");
  }
  if (!ContainsArg(config.extra_args, "--link-remap")) {
    command.push_back("--link-remap");
  }
  command.insert(command.end(), config.extra_args.begin(), config.extra_args.end());
  return command;
}

int RunWorker(const WorkerConfig& config) {
  if (config.operation == "restore" && config.namespace_restore) {
    return RunNamespaceRestore(config);
  }
  const std::vector<std::string> command =
      config.operation == "dump" ? BuildDumpCommand(config) : BuildRestoreCommand(config);
  if (config.operation == "restore") {
    // Host restore needs a real session leader and controlling TTY so shell-job
    // restores inherit the caller's terminal with working job control.
    return ForkExecRestoreWithControllingTty(command, config.tty_fd);
  }
  return ForkExec(command);
}

int RunWorkerMain(int argc, char** argv) {
  const WorkerConfig config = ParseArgs(argc, argv);
  return RunWorker(config);
}

}  // namespace snapshotd
