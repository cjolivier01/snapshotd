#include "src/csrc/worker.h"

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sched.h>
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

bool ContainsArg(const std::vector<std::string>& args, const std::string& token) {
  for (const std::string& arg : args) {
    if (arg == token || arg.rfind(token + "=", 0) == 0) {
      return true;
    }
  }
  return false;
}

void AppendLogLine(const fs::path& path, const std::string& line) {
  EnsureDir(path.parent_path(), 0700);
  std::ofstream output(path, std::ios::out | std::ios::app);
  if (!output.is_open()) {
    throw std::runtime_error("failed to open log for append: " + path.string());
  }
  output << line << "\n";
}

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

std::vector<char*> MakeArgv(const std::vector<std::string>& command) {
  std::vector<char*> argv;
  argv.reserve(command.size() + 1);
  for (const std::string& token : command) {
    argv.push_back(const_cast<char*>(token.c_str()));
  }
  argv.push_back(nullptr);
  return argv;
}

[[noreturn]] void ExecCommand(const std::vector<std::string>& command) {
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

pid_t ResolveHostPidFromLocalPid(pid_t ns_init_host_pid, pid_t local_pid) {
  const auto deadline = std::chrono::steady_clock::now() + kNamespaceRestoreTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    for (pid_t host_pid : ReadChildPids(ns_init_host_pid)) {
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

void MountNewProc() {
  if (mount(nullptr, "/", nullptr, MS_SLAVE | MS_REC, nullptr) != 0) {
    ThrowErrno("mount(/, MS_SLAVE|MS_REC)");
  }
  if (mount("proc", "/proc", "proc", 0, nullptr) != 0) {
    ThrowErrno("mount(/proc)");
  }
}

void PrepareControllingTty() {
  if (isatty(STDIN_FILENO)) {
    if (ioctl(STDIN_FILENO, TIOCSCTTY, 1) != 0) {
      ThrowErrno("ioctl(TIOCSCTTY)");
    }
    return;
  }
  int master_fd = -1;
  int slave_fd = -1;
  if (openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr) != 0) {
    ThrowErrno("openpty");
  }
  if (dup2(slave_fd, STDIN_FILENO) < 0) {
    ThrowErrno("dup2(slave tty)");
  }
  if (ioctl(STDIN_FILENO, TIOCSCTTY, 1) != 0) {
    ThrowErrno("ioctl(TIOCSCTTY)");
  }
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

pid_t FindRestoredLocalPid(pid_t criu_pid) {
  for (pid_t child_pid : ReadChildPids(1)) {
    if (child_pid != criu_pid) {
      return child_pid;
    }
  }
  return 0;
}

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
    PrepareControllingTty();
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
  return ForkExec(command);
}

int RunWorkerMain(int argc, char** argv) {
  const WorkerConfig config = ParseArgs(argc, argv);
  return RunWorker(config);
}

}  // namespace snapshotd
