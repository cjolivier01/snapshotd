/** @file
 *  @brief Policy-enforcing privileged broker for managed checkpoint jobs.
 */

#include "src/csrc/daemon.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

#include "src/csrc/protocol.h"
#include "src/csrc/util.h"

namespace snapshotd {

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_terminate(false);

void SignalHandler(int) {
  g_terminate.store(true);
}

bool UseSystemdSocketActivation() {
  const std::string listen_pid = GetEnv("LISTEN_PID");
  const std::string listen_fds = GetEnv("LISTEN_FDS");
  if (listen_pid.empty() || listen_fds.empty()) {
    return false;
  }
  return static_cast<pid_t>(std::stol(listen_pid)) == getpid() &&
         std::stoi(listen_fds) >= 1;
}

int CreateSocket(const std::string& socket_path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    ThrowErrno("socket(AF_UNIX)");
  }

  fs::path path(socket_path);
  const fs::path parent = path.parent_path();
  if (!parent.empty() && !PathExists(parent)) {
    EnsureDir(parent, 0770);
  }
  unlink(socket_path.c_str());

  sockaddr_un address {};
  address.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(address.sun_path)) {
    close(fd);
    throw std::runtime_error("socket path too long");
  }
  std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path.c_str());
  if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string error = ErrnoMessage("bind(" + socket_path + ")");
    close(fd);
    throw std::runtime_error(error);
  }
  if (chmod(socket_path.c_str(), 0660) != 0) {
    const std::string error = ErrnoMessage("chmod(" + socket_path + ")");
    close(fd);
    throw std::runtime_error(error);
  }
  if (listen(fd, 32) != 0) {
    const std::string error = ErrnoMessage("listen(" + socket_path + ")");
    close(fd);
    throw std::runtime_error(error);
  }
  return fd;
}

std::vector<char*> MakeArgv(const std::vector<std::string>& argv_storage) {
  std::vector<char*> argv;
  argv.reserve(argv_storage.size() + 1);
  for (const std::string& token : argv_storage) {
    argv.push_back(const_cast<char*>(token.c_str()));
  }
  argv.push_back(nullptr);
  return argv;
}

std::vector<char*> MakeEnvp(const std::vector<std::string>& env_storage) {
  std::vector<char*> envp;
  envp.reserve(env_storage.size() + 1);
  for (const std::string& entry : env_storage) {
    envp.push_back(const_cast<char*>(entry.c_str()));
  }
  envp.push_back(nullptr);
  return envp;
}

struct LaunchResult {
  pid_t pid = 0;
  std::string start_time_ticks;
};

enum class LaunchStage {
  kInitGroups = 1,
  kSetGid = 2,
  kSetUid = 3,
  kStdio = 4,
  kChdir = 5,
  kExecve = 6,
};

struct LaunchFailure {
  int stage = 0;
  int error_code = 0;
};

class RequestError : public std::runtime_error {
 public:
  RequestError(std::string code, std::string message)
      : std::runtime_error(std::move(message)), code_(std::move(code)) {}

  const std::string& code() const { return code_; }
  const std::vector<std::pair<std::string, std::string>>& fields() const { return fields_; }

  void AddField(const std::string& key, const std::string& value) {
    fields_.emplace_back(key, value);
  }

 private:
  std::string code_;
  std::vector<std::pair<std::string, std::string>> fields_;
};

void SetCloseOnExec(int fd) {
  const int flags = fcntl(fd, F_GETFD);
  if (flags < 0) {
    ThrowErrno("fcntl(F_GETFD)");
  }
  if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
    ThrowErrno("fcntl(F_SETFD)");
  }
}

LaunchResult LaunchManagedJob(
    const PeerCred& peer,
    const std::vector<std::string>& argv,
    const std::string& cwd) {
  if (argv.empty()) {
    throw std::runtime_error("run request requires at least one argv token");
  }
  if (!IsAbsolutePath(argv.front())) {
    throw std::runtime_error("run executable must be absolute");
  }
  if (!IsAbsolutePath(cwd)) {
    throw std::runtime_error("run cwd must be absolute");
  }
  // Reject obviously privileged binaries before exec so the broker never
  // launches a managed job that can elevate itself through file metadata.
  ValidateManagedExecutable(argv.front());

  struct passwd* pwd = getpwuid(peer.uid);
  const std::string home = pwd && pwd->pw_dir ? pwd->pw_dir : cwd;
  const std::string user = pwd && pwd->pw_name ? pwd->pw_name : UidToString(peer.uid);
  const std::string shell = pwd && pwd->pw_shell && pwd->pw_shell[0] != '\0'
                                ? pwd->pw_shell
                                : "/bin/sh";

  int error_pipe[2] = {-1, -1};
  if (pipe(error_pipe) != 0) {
    ThrowErrno("pipe");
  }
  SetCloseOnExec(error_pipe[0]);
  SetCloseOnExec(error_pipe[1]);

  const pid_t child = fork();
  if (child < 0) {
    close(error_pipe[0]);
    close(error_pipe[1]);
    ThrowErrno("fork");
  }
  if (child == 0) {
    close(error_pipe[0]);
    auto fail = [&](LaunchStage stage, int error_code) {
      const LaunchFailure failure = {
          static_cast<int>(stage),
          error_code,
      };
      (void)!write(error_pipe[1], &failure, sizeof(failure));
      _exit(127);
    };
    if (geteuid() == 0) {
      // Managed jobs run as the caller, even though the daemon itself is root.
      // The worker later regains privilege only for the narrow CRIU operation.
      if (pwd != nullptr) {
        if (initgroups(pwd->pw_name, peer.gid) != 0) {
          fail(LaunchStage::kInitGroups, errno);
        }
      }
      if (setgid(peer.gid) != 0) {
        fail(LaunchStage::kSetGid, errno);
      }
      if (setuid(peer.uid) != 0) {
        fail(LaunchStage::kSetUid, errno);
      }
    }
    const int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull < 0) {
      fail(LaunchStage::kStdio, errno);
    }
    if (dup2(devnull, STDIN_FILENO) < 0 || dup2(devnull, STDOUT_FILENO) < 0 ||
        dup2(devnull, STDERR_FILENO) < 0) {
      fail(LaunchStage::kStdio, errno);
    }
    if (devnull > STDERR_FILENO) {
      close(devnull);
    }
    if (chdir(cwd.c_str()) != 0) {
      fail(LaunchStage::kChdir, errno);
    }
    std::vector<std::string> env_storage = {
        "HOME=" + home,
        "LOGNAME=" + user,
        "PATH=/usr/local/bin:/usr/bin:/bin",
        "SHELL=" + shell,
        "USER=" + user,
    };
    std::vector<char*> argvp = MakeArgv(argv);
    std::vector<char*> envp = MakeEnvp(env_storage);
    execve(argv.front().c_str(), argvp.data(), envp.data());
    fail(LaunchStage::kExecve, errno);
  }

  close(error_pipe[1]);
  // The post-fork start-time token is persisted alongside the pid so later
  // privileged operations can reject stale job ids after PID reuse.
  const std::string start_time_ticks = ReadProcessStartTimeTicks(child);
  LaunchFailure failure {};
  const ssize_t count = read(error_pipe[0], &failure, sizeof(failure));
  close(error_pipe[0]);
  if (count < 0) {
    ThrowErrno("read(exec status pipe)");
  }
  if (count > 0) {
    int status = 0;
    (void)!waitpid(child, &status, 0);
    errno = failure.error_code;
    std::string context = "launch child";
    switch (static_cast<LaunchStage>(failure.stage)) {
      case LaunchStage::kInitGroups:
        context = "initgroups(" + user + ")";
        break;
      case LaunchStage::kSetGid:
        context = "setgid(" + GidToString(peer.gid) + ")";
        break;
      case LaunchStage::kSetUid:
        context = "setuid(" + UidToString(peer.uid) + ")";
        break;
      case LaunchStage::kStdio:
        context = "redirect stdio to /dev/null";
        break;
      case LaunchStage::kChdir:
        context = "chdir(" + cwd + ")";
        break;
      case LaunchStage::kExecve:
        context = "execve(" + argv.front() + ")";
        break;
    }
    ThrowErrno(context);
  }
  if (!ProcessIdentityMatches(child, peer.uid, peer.gid, start_time_ticks)) {
    throw std::runtime_error("managed job identity did not stabilize after exec");
  }
  std::string launch_reason;
  if (!ProcessMatchesPeerSecurity(child, peer.uid, peer.gid, start_time_ticks, &launch_reason)) {
    kill(child, SIGKILL);
    (void)!waitpid(child, nullptr, 0);
    throw std::runtime_error("managed job failed security validation after exec: " + launch_reason);
  }
  return {child, start_time_ticks};
}

std::string DefaultCriuNsPath(const std::string& criu_bin) {
  const fs::path path(criu_bin);
  return (path.parent_path() / "criu-ns").string();
}

bool MessageFlagEnabled(const Message& request, const std::string& key) {
  const std::string value = request.Get(key);
  return value == "1" || value == "true" || value == "yes";
}

std::vector<std::string> ValidateExtraArgs(const std::vector<std::string>& extra_args) {
  std::vector<std::string> validated;
  for (std::size_t index = 0; index < extra_args.size(); ++index) {
    const std::string& token = extra_args[index];
    if (token == "--link-remap" || token == "--tcp-established" ||
        token == "--tcp-close" || token == "--file-locks" ||
        token.rfind("--ghost-limit=", 0) == 0) {
      validated.push_back(token);
      continue;
    }
    if (token == "--ghost-limit") {
      if (index + 1 >= extra_args.size()) {
        throw std::runtime_error("snapshotd ghost-limit flag requires a value");
      }
      validated.push_back(token);
      validated.push_back(extra_args[++index]);
      continue;
    }
    if (token == "--external") {
      if (index + 1 >= extra_args.size() || extra_args[index + 1].rfind("tty[", 0) != 0) {
        throw std::runtime_error("invalid snapshotd external tty argument");
      }
      validated.push_back(token);
      validated.push_back(extra_args[++index]);
      continue;
    }
    if (token == "--inherit-fd") {
      if (index + 1 >= extra_args.size() || extra_args[index + 1].rfind("fd[", 0) != 0) {
        throw std::runtime_error("invalid snapshotd inherit-fd argument");
      }
      validated.push_back(token);
      validated.push_back(extra_args[++index]);
      continue;
    }
    throw std::runtime_error("unsafe CRIU argument for snapshotd request: " + token);
  }
  return validated;
}

JobRecord CreatePidOwnedJob(Store* store, const PeerCred& peer, pid_t target_pid) {
  if (target_pid <= 1) {
    throw std::runtime_error("checkpoint pid must be > 1");
  }
  // This is the narrow self-checkpoint bridge used by downstream runtimes. It
  // still records a managed job and pins ownership to the connected peer uid.
  const ProcessIdentity identity = ReadProcessIdentity(target_pid);
  std::string reason;
  if (!ProcessMatchesPeerSecurity(
          target_pid, peer.uid, peer.gid, identity.start_time_ticks, &reason)) {
    throw std::runtime_error("checkpoint target is not a safe peer-owned process: " + reason);
  }
  const std::string executable = ReadProcessExecutablePath(target_pid);
  const std::string cwd = ReadProcessWorkingDirectory(target_pid);
  std::vector<std::string> argv = ReadProcessCommandLine(target_pid);
  if (argv.empty()) {
    argv.push_back(executable);
  }
  JobRecord job = store->CreateJob(
      peer.uid,
      peer.gid,
      target_pid,
      identity.start_time_ticks,
      executable,
      cwd,
      JoinCommandLine(argv));
  store->SaveJob(job);
  return job;
}

fs::path ExportCheckpointArtifacts(const Store& store, const JobRecord& job, const CheckpointRecord& checkpoint) {
  const fs::path checkpoint_dir =
      store.CheckpointDir(job.owner_uid, job.job_id, checkpoint.checkpoint_id);
  const fs::path export_dir =
      store.ExportCheckpointDir(job.owner_uid, job.job_id, checkpoint.checkpoint_id);
  EnsureDir(store.UserDir(job.owner_uid), 0755);
  EnsureDir(store.ExportsDir(job.owner_uid), 0755);
  EnsureDir(store.ExportsDir(job.owner_uid) / job.job_id, 0755);
  RemoveTree(export_dir);
  EnsureDir(export_dir, 0700);
  CopyTree(checkpoint_dir / "images", export_dir / "images");
  CopyTree(checkpoint_dir / "logs", export_dir / "logs");
  ChownTree(export_dir, job.owner_uid, job.owner_gid);
  return export_dir;
}

Message CheckpointSuccessResponse(
    const JobRecord& job,
    const CheckpointRecord& checkpoint,
    const fs::path& export_dir) {
  Message response;
  response.command = "ok";
  response.AddField("job_id", job.job_id);
  response.AddField("checkpoint_id", checkpoint.checkpoint_id);
  response.AddField("dump_log", (export_dir / "logs" / "dump.log").string());
  response.AddField("export_images", (export_dir / "images").string());
  return response;
}

std::vector<std::string> BuildWorkerCommand(
    const DaemonConfig& config,
    const Store& store,
    const JobRecord& job,
    const CheckpointRecord& checkpoint,
    const std::string& operation,
    const std::vector<std::string>& extra_args,
    bool namespace_mode) {
  const fs::path job_dir = store.JobDir(job.owner_uid, job.job_id);
  const fs::path checkpoint_dir =
      store.CheckpointDir(job.owner_uid, job.job_id, checkpoint.checkpoint_id);
  std::vector<std::string> command = {
      config.worker_path,
      "--operation",
      operation,
      "--state-dir",
      store.state_root().string(),
      "--job-dir",
      job_dir.string(),
      "--checkpoint-dir",
      checkpoint_dir.string(),
      "--criu-bin",
      config.criu_bin,
      "--criu-ns-bin",
      config.criu_ns_bin,
  };
  if (operation == "dump") {
    command.push_back("--pid");
    command.push_back(PidToString(job.pid));
    if (namespace_mode) {
      command.push_back("--namespace-dump");
    }
  } else if (namespace_mode) {
    command.push_back("--namespace-restore");
  }
  for (const std::string& extra_arg : extra_args) {
    command.push_back("--extra-arg");
    command.push_back(extra_arg);
  }
  return command;
}

void WaitForWorkerSuccess(
    const std::vector<std::string>& command,
    std::chrono::seconds timeout) {
  const pid_t child = fork();
  if (child < 0) {
    ThrowErrno("fork");
  }
  if (child == 0) {
    // Put the worker and anything it spawns into its own process group so a
    // timeout can terminate the whole subtree, not just the top-level worker.
    if (setpgid(0, 0) != 0) {
      _exit(127);
    }
    std::vector<char*> argv = MakeArgv(command);
    execv(command.front().c_str(), argv.data());
    _exit(127);
  }
  (void)!setpgid(child, child);

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    int status = 0;
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno("waitpid");
    }
    if (waited == child) {
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("worker invocation failed: " + JoinCommandLine(command));
      }
      return;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      (void)!kill(-child, SIGTERM);
      const auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (true) {
        const pid_t timed_out_wait = waitpid(child, &status, WNOHANG);
        if (timed_out_wait == child) {
          break;
        }
        if (timed_out_wait < 0) {
          if (errno == EINTR) {
            continue;
          }
          if (errno == ECHILD) {
            break;
          }
          ThrowErrno("waitpid");
        }
        if (std::chrono::steady_clock::now() >= kill_deadline) {
          (void)!kill(-child, SIGKILL);
          if (waitpid(child, &status, 0) < 0 && errno != ECHILD) {
            ThrowErrno("waitpid");
          }
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      throw std::runtime_error(
          "worker invocation timed out after " + std::to_string(timeout.count()) +
          "s: " + JoinCommandLine(command));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void UpdateJobLiveness(const Store& store, JobRecord* job) {
  std::string new_state = "exited";
  std::string reason;
  if (ProcessMatchesPeerSecurity(
          job->pid, job->owner_uid, job->owner_gid, job->start_time_ticks, &reason)) {
    new_state = "running";
  } else if (ProcessIdentityMatches(
                 job->pid, job->owner_uid, job->owner_gid, job->start_time_ticks)) {
    // The job still maps to the same PID/start-time tuple, but its credentials
    // drifted away from the caller's unprivileged state.
    new_state = "unsafe";
  } else if (IsProcessAlive(job->pid)) {
    new_state = "stale";
  }
  if (job->state != new_state) {
    job->state = new_state;
    store.SaveJob(*job);
  }
}

void AdoptManagedPid(JobRecord* job, pid_t pid) {
  const ProcessIdentity identity = ReadProcessIdentity(pid);
  std::string reason;
  if (!ProcessMatchesPeerSecurity(
          identity.pid, job->owner_uid, job->owner_gid, identity.start_time_ticks, &reason)) {
    throw std::runtime_error("restored pid failed security validation: " + reason);
  }
  job->pid = identity.pid;
  job->start_time_ticks = identity.start_time_ticks;
  job->state = "running";
}

Message StatusResponse(const JobRecord& job) {
  Message response;
  response.command = "ok";
  response.AddField("job_id", job.job_id);
  response.AddField("owner_uid", UidToString(job.owner_uid));
  response.AddField("owner_gid", GidToString(job.owner_gid));
  response.AddField("pid", PidToString(job.pid));
  response.AddField("state", job.state);
  response.AddField("latest_checkpoint", job.latest_checkpoint);
  response.AddField("command_line", job.command_line);
  return response;
}

Message ErrorResponse(const std::string& code, const std::string& message) {
  Message response;
  response.command = "error";
  response.AddField("code", code);
  response.AddField("message", message);
  return response;
}

Message ErrorResponse(const RequestError& error) {
  Message response = ErrorResponse(error.code(), error.what());
  for (const auto& [key, value] : error.fields()) {
    response.AddField(key, value);
  }
  return response;
}

DaemonConfig ParseArgs(int argc, char** argv) {
  DaemonConfig config;
  config.socket_path = "/run/snapshotd.sock";
  config.state_dir = "/var/lib/snapshotd";
  config.worker_path = "/usr/libexec/snapshotd/snapshot-worker";
  config.criu_bin = "/usr/local/sbin/criu";
  config.criu_ns_bin = DefaultCriuNsPath(config.criu_bin);
  config.worker_timeout_seconds = 600;

  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    auto require_value = [&](const std::string& flag) -> std::string {
      if (index + 1 >= argc) {
        throw std::runtime_error("missing value for " + flag);
      }
      ++index;
      return argv[index];
    };
    if (arg == "--socket-path") {
      config.socket_path = require_value(arg);
    } else if (arg == "--state-dir") {
      config.state_dir = require_value(arg);
    } else if (arg == "--worker-path") {
      config.worker_path = require_value(arg);
    } else if (arg == "--criu-bin") {
      config.criu_bin = require_value(arg);
      config.criu_ns_bin = DefaultCriuNsPath(config.criu_bin);
    } else if (arg == "--criu-ns-bin") {
      config.criu_ns_bin = require_value(arg);
    } else if (arg == "--worker-timeout-seconds") {
      config.worker_timeout_seconds = std::stoi(require_value(arg));
      if (config.worker_timeout_seconds <= 0) {
        throw std::runtime_error("worker timeout must be > 0 seconds");
      }
    } else if (arg == "--ready-file") {
      config.ready_file = require_value(arg);
    } else {
      throw std::runtime_error("unknown daemon flag: " + arg);
    }
  }
  return config;
}

}  // namespace

Message HandleRequest(
    const Message& request,
    const PeerCred& peer,
    const DaemonConfig& config,
    Store* store) {
  if (request.command == "run") {
    ValidateAllowedFields(request, {"arg", "cwd"});
    const std::vector<std::string> args = request.GetAll("arg");
    const std::string cwd = request.Get("cwd");
    const LaunchResult launched = LaunchManagedJob(peer, args, cwd);
    JobRecord job = store->CreateJob(
        peer.uid,
        peer.gid,
        launched.pid,
        launched.start_time_ticks,
        args.front(),
        cwd,
        JoinCommandLine(args));
    store->SaveJob(job);
    Message response;
    response.command = "ok";
    response.AddField("job_id", job.job_id);
    response.AddField("pid", PidToString(job.pid));
    return response;
  }

  if (request.command == "status") {
    ValidateAllowedFields(request, {"job_id"});
    const std::string job_id = request.Get("job_id");
    RequireSafeId(job_id, "job_id");
    JobRecord job = store->LoadJob(peer.uid, job_id);
    AuthorizeJobAccess(job, peer.uid);
    UpdateJobLiveness(*store, &job);
    return StatusResponse(job);
  }

  if (request.command == "checkpoint") {
    ValidateAllowedFields(request, {"job_id", "extra_arg", "namespace_dump"});
    const std::string job_id = request.Get("job_id");
    RequireSafeId(job_id, "job_id");
    JobRecord job = store->LoadJob(peer.uid, job_id);
    AuthorizeJobAccess(job, peer.uid);
    UpdateJobLiveness(*store, &job);
    if (job.state != "running") {
      throw std::runtime_error("cannot checkpoint a non-running managed job");
    }
    std::string reason;
    if (!ProcessMatchesPeerSecurity(
            job.pid, job.owner_uid, job.owner_gid, job.start_time_ticks, &reason)) {
      UpdateJobLiveness(*store, &job);
      throw std::runtime_error("managed job is not safe to checkpoint: " + reason);
    }
    // Revalidate pid + start_time + unprivileged credentials before the
    // privileged worker runs so a stale or elevated job id cannot drift onto a
    // reused or newly-privileged pid.
    CheckpointRecord checkpoint = store->CreateCheckpoint(job);
    store->SaveCheckpoint(job, checkpoint);
    const std::vector<std::string> extra_args =
        ValidateExtraArgs(request.GetAll("extra_arg"));
    const bool namespace_dump = MessageFlagEnabled(request, "namespace_dump");
    try {
      WaitForWorkerSuccess(
          BuildWorkerCommand(
              config, *store, job, checkpoint, "dump", extra_args, namespace_dump),
          std::chrono::seconds(config.worker_timeout_seconds));
    } catch (const std::exception& error) {
      checkpoint.state = "failed";
      store->SaveCheckpoint(job, checkpoint);
      const fs::path export_dir = ExportCheckpointArtifacts(*store, job, checkpoint);
      RequestError request_error("checkpoint_failed", error.what());
      request_error.AddField("job_id", job.job_id);
      request_error.AddField("checkpoint_id", checkpoint.checkpoint_id);
      request_error.AddField("dump_log", (export_dir / "logs" / "dump.log").string());
      request_error.AddField("export_images", (export_dir / "images").string());
      throw request_error;
    }
    checkpoint.state = "ready";
    store->SaveCheckpoint(job, checkpoint);
    job.latest_checkpoint = checkpoint.checkpoint_id;
    store->SaveJob(job);
    const fs::path export_dir = ExportCheckpointArtifacts(*store, job, checkpoint);
    return CheckpointSuccessResponse(job, checkpoint, export_dir);
  }

  if (request.command == "checkpoint-pid") {
    ValidateAllowedFields(request, {"pid", "extra_arg", "namespace_dump"});
    const pid_t target_pid = static_cast<pid_t>(std::stol(request.Get("pid")));
    JobRecord job = CreatePidOwnedJob(store, peer, target_pid);
    CheckpointRecord checkpoint = store->CreateCheckpoint(job);
    store->SaveCheckpoint(job, checkpoint);
    const std::vector<std::string> extra_args =
        ValidateExtraArgs(request.GetAll("extra_arg"));
    const bool namespace_dump = MessageFlagEnabled(request, "namespace_dump");
    std::string reason;
    if (!ProcessMatchesPeerSecurity(
            job.pid, job.owner_uid, job.owner_gid, job.start_time_ticks, &reason)) {
      throw std::runtime_error("checkpoint target is not safe to checkpoint: " + reason);
    }
    try {
      WaitForWorkerSuccess(
          BuildWorkerCommand(
              config, *store, job, checkpoint, "dump", extra_args, namespace_dump),
          std::chrono::seconds(config.worker_timeout_seconds));
    } catch (const std::exception& error) {
      checkpoint.state = "failed";
      store->SaveCheckpoint(job, checkpoint);
      const fs::path export_dir = ExportCheckpointArtifacts(*store, job, checkpoint);
      RequestError request_error("checkpoint_failed", error.what());
      request_error.AddField("job_id", job.job_id);
      request_error.AddField("checkpoint_id", checkpoint.checkpoint_id);
      request_error.AddField("dump_log", (export_dir / "logs" / "dump.log").string());
      request_error.AddField("export_images", (export_dir / "images").string());
      throw request_error;
    }
    checkpoint.state = "ready";
    store->SaveCheckpoint(job, checkpoint);
    job.latest_checkpoint = checkpoint.checkpoint_id;
    store->SaveJob(job);
    const fs::path export_dir = ExportCheckpointArtifacts(*store, job, checkpoint);
    return CheckpointSuccessResponse(job, checkpoint, export_dir);
  }

  if (request.command == "restore") {
    ValidateAllowedFields(request, {"job_id", "checkpoint_id", "extra_arg", "namespace_restore"});
    const std::string job_id = request.Get("job_id");
    RequireSafeId(job_id, "job_id");
    JobRecord job = store->LoadJob(peer.uid, job_id);
    AuthorizeJobAccess(job, peer.uid);
    const std::string checkpoint_id =
        store->ResolveCheckpointId(job, request.Get("checkpoint_id"));
    CheckpointRecord checkpoint = store->LoadCheckpoint(job, checkpoint_id);
    const std::vector<std::string> extra_args =
        ValidateExtraArgs(request.GetAll("extra_arg"));
    const bool namespace_restore = MessageFlagEnabled(request, "namespace_restore");
    try {
      WaitForWorkerSuccess(
          BuildWorkerCommand(
              config,
              *store,
              job,
              checkpoint,
              "restore",
              extra_args,
              namespace_restore),
          std::chrono::seconds(config.worker_timeout_seconds));
    } catch (const std::exception& error) {
      checkpoint.state = "restore-failed";
      store->SaveCheckpoint(job, checkpoint);
      const fs::path export_dir = ExportCheckpointArtifacts(*store, job, checkpoint);
      RequestError request_error("restore_failed", error.what());
      request_error.AddField("job_id", job.job_id);
      request_error.AddField("checkpoint_id", checkpoint.checkpoint_id);
      request_error.AddField("restore_log", (export_dir / "logs" / "restore.log").string());
      throw request_error;
    }
    const fs::path checkpoint_dir =
        store->CheckpointDir(job.owner_uid, job.job_id, checkpoint.checkpoint_id);
    const fs::path pidfile = checkpoint_dir / "restore.pid";
    if (PathExists(pidfile)) {
      checkpoint.restored_pid = ParseKeyValueText("pid=" + ReadTextFile(pidfile))["pid"];
      AdoptManagedPid(&job, static_cast<pid_t>(std::stol(checkpoint.restored_pid)));
      store->SaveJob(job);
    } else {
      checkpoint.restored_pid.clear();
    }
    checkpoint.state = "restored";
    store->SaveCheckpoint(job, checkpoint);
    const fs::path export_dir = ExportCheckpointArtifacts(*store, job, checkpoint);

    Message response;
    response.command = "ok";
    response.AddField("job_id", job.job_id);
    response.AddField("checkpoint_id", checkpoint.checkpoint_id);
    response.AddField("restored_pid", checkpoint.restored_pid);
    response.AddField("restore_log", (export_dir / "logs" / "restore.log").string());
    return response;
  }

  throw std::runtime_error("unsupported request command: " + request.command);
}

int RunDaemon(const DaemonConfig& config) {
  Store store(config.state_dir);
  store.Initialize();

  signal(SIGTERM, SignalHandler);
  signal(SIGINT, SignalHandler);
  signal(SIGPIPE, SIG_IGN);

  const bool inherited_socket = UseSystemdSocketActivation();
  const int listen_fd = inherited_socket ? 3 : CreateSocket(config.socket_path);
  SetCloseOnExec(listen_fd);
  if (!config.ready_file.empty()) {
    WriteTextFile(config.ready_file, "ready\n");
  }

  while (!g_terminate.load()) {
    pollfd poll_fd {};
    poll_fd.fd = listen_fd;
    poll_fd.events = POLLIN;
    const int poll_result = poll(&poll_fd, 1, 250);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno("poll");
    }
    if (poll_result == 0) {
      continue;
    }
    int client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (client_fd < 0) {
      if (errno == ENOSYS || errno == EINVAL) {
        client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd >= 0) {
          SetCloseOnExec(client_fd);
        }
      }
    }
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno("accept");
    }
    try {
      // Authorization is per-connection, derived from the kernel rather than
      // any client-supplied uid/pid fields.
      const PeerCred peer = GetPeerCred(client_fd);
      const Message request = ReceiveMessage(client_fd);
      const Message response = HandleRequest(request, peer, config, &store);
      try {
        SendMessage(client_fd, response);
      } catch (...) {
      }
    } catch (const RequestError& error) {
      try {
        SendMessage(client_fd, ErrorResponse(error));
      } catch (...) {
      }
    } catch (const std::exception& error) {
      try {
        SendMessage(client_fd, ErrorResponse("request_failed", error.what()));
      } catch (...) {
      }
    }
    close(client_fd);
  }

  if (!inherited_socket) {
    close(listen_fd);
    unlink(config.socket_path.c_str());
  }
  return 0;
}

int RunDaemonMain(int argc, char** argv) {
  const DaemonConfig config = ParseArgs(argc, argv);
  return RunDaemon(config);
}

}  // namespace snapshotd
