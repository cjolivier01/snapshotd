#include <sys/socket.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "src/csrc/client.h"
#include "src/csrc/protocol.h"
#include "src/csrc/util.h"

namespace fs = std::filesystem;

namespace {

struct CommandResult {
  int exit_code = 0;
  std::string stdout_text;
  std::string stderr_text;
};

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

fs::path Runfile(const std::string& relative_path) {
  const char* test_srcdir = getenv("TEST_SRCDIR");
  if (test_srcdir == nullptr) {
    throw std::runtime_error("TEST_SRCDIR is not set");
  }
  return fs::path(test_srcdir) / "snapshotd" / relative_path;
}

fs::path MakeTempDir(const std::string& prefix) {
  const fs::path path = fs::temp_directory_path() / snapshotd::GenerateId(prefix);
  snapshotd::EnsureDir(path, 0700);
  return path;
}

void WriteExecutable(const fs::path& path, const std::string& text) {
  snapshotd::WriteTextFile(path, text);
  if (chmod(path.c_str(), 0755) != 0) {
    throw std::runtime_error("chmod failed for " + path.string());
  }
}

std::map<std::string, std::string> ParseOutputMap(const std::string& text) {
  std::map<std::string, std::string> values;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty()) {
      continue;
    }
    const std::size_t delimiter = line.find('=');
    if (delimiter == std::string::npos) {
      continue;
    }
    values[line.substr(0, delimiter)] = line.substr(delimiter + 1);
  }
  return values;
}

CommandResult RunCommand(
    const std::vector<std::string>& argv,
    const std::map<std::string, std::string>& extra_env = {}) {
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
    throw std::runtime_error("pipe failed");
  }

  const pid_t child = fork();
  if (child < 0) {
    throw std::runtime_error("fork failed");
  }
  if (child == 0) {
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    for (const auto& [key, value] : extra_env) {
      setenv(key.c_str(), value.c_str(), 1);
    }
    std::vector<char*> exec_argv;
    exec_argv.reserve(argv.size() + 1);
    for (const std::string& token : argv) {
      exec_argv.push_back(const_cast<char*>(token.c_str()));
    }
    exec_argv.push_back(nullptr);
    execv(argv.front().c_str(), exec_argv.data());
    _exit(127);
  }

  close(stdout_pipe[1]);
  close(stderr_pipe[1]);

  auto read_all = [](int fd) {
    std::string text;
    char buffer[4096];
    while (true) {
      const ssize_t count = read(fd, buffer, sizeof(buffer));
      if (count == 0) {
        break;
      }
      if (count < 0) {
        throw std::runtime_error("read failed");
      }
      text.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);
    return text;
  };

  CommandResult result;
  result.stdout_text = read_all(stdout_pipe[0]);
  result.stderr_text = read_all(stderr_pipe[0]);
  int status = 0;
  if (waitpid(child, &status, 0) < 0) {
    throw std::runtime_error("waitpid failed");
  }
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  return result;
}

pid_t SpawnDaemon(
    const fs::path& daemon_bin,
    const fs::path& socket_path,
    const fs::path& state_dir,
    const fs::path& worker_bin,
    const fs::path& fake_criu,
    const fs::path& fake_criu_ns,
    const fs::path& ready_file,
    const std::map<std::string, std::string>& env,
    int worker_timeout_seconds = 0) {
  const pid_t child = fork();
  if (child < 0) {
    throw std::runtime_error("fork failed");
  }
  if (child == 0) {
    for (const auto& [key, value] : env) {
      setenv(key.c_str(), value.c_str(), 1);
    }
    std::vector<std::string> argv = {
        daemon_bin.string(),
        "--socket-path",
        socket_path.string(),
        "--state-dir",
        state_dir.string(),
        "--worker-path",
        worker_bin.string(),
        "--criu-bin",
        fake_criu.string(),
        "--criu-ns-bin",
        fake_criu_ns.string(),
    };
    if (worker_timeout_seconds > 0) {
      argv.push_back("--worker-timeout-seconds");
      argv.push_back(std::to_string(worker_timeout_seconds));
    }
    argv.push_back("--ready-file");
    argv.push_back(ready_file.string());
    std::vector<char*> exec_argv;
    for (const std::string& token : argv) {
      exec_argv.push_back(const_cast<char*>(token.c_str()));
    }
    exec_argv.push_back(nullptr);
    execv(exec_argv.front(), exec_argv.data());
    _exit(127);
  }
  return child;
}

void SendAndClose(const fs::path& socket_path, const snapshotd::Message& request) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    throw std::runtime_error("socket(AF_UNIX) failed");
  }

  sockaddr_un address {};
  address.sun_family = AF_UNIX;
  std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path.c_str());
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(fd);
    throw std::runtime_error("connect failed");
  }
  snapshotd::SendMessage(fd, request);
  close(fd);
}

void WaitForPath(const fs::path& path, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (fs::exists(path)) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  throw std::runtime_error("timed out waiting for " + path.string());
}

void KillAndWait(pid_t pid) {
  if (pid <= 0) {
    return;
  }
  kill(pid, SIGTERM);
  int status = 0;
  waitpid(pid, &status, 0);
}

std::size_t CountNonStandardFds(pid_t pid) {
  const fs::path fd_dir = fs::path("/proc") / std::to_string(pid) / "fd";
  std::size_t count = 0;
  for (const auto& entry : fs::directory_iterator(fd_dir)) {
    const std::string name = entry.path().filename().string();
    if (name == "." || name == "..") {
      continue;
    }
    const int fd = std::stoi(name);
    if (fd > STDERR_FILENO) {
      ++count;
    }
  }
  return count;
}

std::string DescriptorTarget(pid_t pid, int fd) {
  return fs::read_symlink(fs::path("/proc") / std::to_string(pid) / "fd" /
                          std::to_string(fd))
      .string();
}

void TestDaemonFlowAndMaliciousInputs() {
  const fs::path temp_dir = MakeTempDir("daemonit");
  const fs::path socket_path = temp_dir / "snapshotd.sock";
  const fs::path state_dir = temp_dir / "state";
  const fs::path ready_file = temp_dir / "ready";
  const fs::path call_log = temp_dir / "fake-criu.calls.log";
  const fs::path env_log = temp_dir / "fake-criu.env.log";
  const fs::path fake_criu = temp_dir / "fake-criu.sh";
  const fs::path fake_criu_ns = temp_dir / "criu-ns";
  const fs::path daemon_bin = Runfile("src/csrc/snapshotd");
  const fs::path worker_bin = Runfile("src/csrc/snapshot-worker");
  const fs::path ctl_bin = Runfile("src/csrc/snapshotctl");

  WriteExecutable(
      fake_criu,
      "#!/usr/bin/env bash\n"
      "set -eu\n"
      "call_log='" + call_log.string() + "'\n"
      "env_log='" + env_log.string() + "'\n"
      "{\n"
      "  printf 'argv0=%s\\n' \"$0\"\n"
      "  for arg in \"$@\"; do printf 'arg=%s\\n' \"$arg\"; done\n"
      "  printf '%s\\n' '---'\n"
      "} >> \"$call_log\"\n"
      "env | sort >> \"$env_log\"\n"
      "printf '%s\\n' '---' >> \"$env_log\"\n"
      "mode=\"${1:-}\"\n"
      "pidfile=''\n"
      "logfile=''\n"
      "while [ \"$#\" -gt 0 ]; do\n"
      "  if [ \"$1\" = '--pidfile' ]; then\n"
      "    pidfile=\"$2\"\n"
      "    shift 2\n"
      "    continue\n"
      "  fi\n"
      "  if [ \"$1\" = '--log-file' ]; then\n"
      "    logfile=\"$2\"\n"
      "    shift 2\n"
      "    continue\n"
      "  fi\n"
      "  shift\n"
      "done\n"
      "if [ -n \"$logfile\" ]; then\n"
      "  mkdir -p \"$(dirname \"$logfile\")\"\n"
      "  printf '%s\\n' 'fake-criu log' > \"$logfile\"\n"
      "fi\n"
      "if [ \"$mode\" = 'restore' ]; then\n"
      "  sleep 60 &\n"
      "fi\n"
      "if [ -n \"$pidfile\" ]; then\n"
      "  mkdir -p \"$(dirname \"$pidfile\")\"\n"
      "  printf '%s\\n' \"$!\" > \"$pidfile\"\n"
      "fi\n");
  WriteExecutable(fake_criu_ns, snapshotd::ReadTextFile(fake_criu));

  const pid_t daemon_pid = SpawnDaemon(
      daemon_bin,
      socket_path,
      state_dir,
      worker_bin,
      fake_criu,
      fake_criu_ns,
      ready_file,
      {{"CRIU_CONFIG_FILE", "/tmp/evil.conf"}});
  pid_t job_pid = 0;
  pid_t restored_pid = 0;
  try {
    WaitForPath(ready_file, std::chrono::milliseconds(5000));
    WaitForPath(socket_path, std::chrono::milliseconds(5000));

    CommandResult run_result = RunCommand(
        {ctl_bin.string(), "--socket-path", socket_path.string(), "run", "--", "/bin/sleep", "30"});
    Expect(run_result.exit_code == 0, "snapshotctl run failed: " + run_result.stderr_text);
    const auto run_map = ParseOutputMap(run_result.stdout_text);
    const std::string job_id = run_map.at("job_id");
    job_pid = static_cast<pid_t>(std::stol(run_map.at("pid")));
    Expect(snapshotd::IsSafeId(job_id), "job id should be safe");
    Expect(job_pid > 1, "expected managed job pid");
    Expect(CountNonStandardFds(job_pid) == 0,
           "managed job inherited unexpected file descriptors");
    Expect(DescriptorTarget(job_pid, STDIN_FILENO) == "/dev/null",
           "managed job stdin should be detached to /dev/null");
    Expect(DescriptorTarget(job_pid, STDOUT_FILENO) == "/dev/null",
           "managed job stdout should be detached to /dev/null");
    Expect(DescriptorTarget(job_pid, STDERR_FILENO) == "/dev/null",
           "managed job stderr should be detached to /dev/null");

    CommandResult status_result =
        RunCommand({ctl_bin.string(), "--socket-path", socket_path.string(), "status", job_id});
    Expect(status_result.exit_code == 0, "status command failed");
    const auto status_map = ParseOutputMap(status_result.stdout_text);
    Expect(status_map.at("state") == "running", "expected running job status");

    CommandResult missing_exec_result = RunCommand(
        {ctl_bin.string(),
         "--socket-path",
         socket_path.string(),
         "run",
         "--",
         (temp_dir / "missing-command").string()});
    Expect(missing_exec_result.exit_code != 0, "missing executable should fail");

    const fs::path setuid_script = temp_dir / "setuid-script.sh";
    WriteExecutable(setuid_script, "#!/usr/bin/env bash\nexit 0\n");
    if (chmod(setuid_script.c_str(), 04755) != 0) {
      throw std::runtime_error("chmod 04755 failed for " + setuid_script.string());
    }
    CommandResult setuid_run_result = RunCommand(
        {ctl_bin.string(), "--socket-path", socket_path.string(), "run", "--", setuid_script.string()});
    Expect(setuid_run_result.exit_code != 0, "setuid executable should be rejected");
    Expect(
        setuid_run_result.stderr_text.find("setuid or setgid") != std::string::npos,
        "setuid rejection should explain why the executable is unsafe");

    snapshotd::Client client(socket_path.string());
    snapshotd::Message bad_request;
    bad_request.command = "checkpoint";
    bad_request.AddField("job_id", job_id);
    bad_request.AddField("criu_arg", "--action-script=/tmp/evil");
    const snapshotd::Message bad_response = client.Request(bad_request);
    Expect(bad_response.command == "error", "unexpected success for unknown field");

    snapshotd::Message path_traversal;
    path_traversal.command = "status";
    path_traversal.AddField("job_id", "../etc/passwd");
    const snapshotd::Message traversal_response = client.Request(path_traversal);
    Expect(traversal_response.command == "error", "path traversal should fail");

    snapshotd::Message relative_run;
    relative_run.command = "run";
    relative_run.AddField("cwd", temp_dir.string());
    relative_run.AddField("arg", "relative-command");
    const snapshotd::Message relative_response = client.Request(relative_run);
    Expect(relative_response.command == "error", "relative executable should fail");

    snapshotd::Message foreign_pid_request;
    foreign_pid_request.command = "checkpoint-pid";
    foreign_pid_request.AddField("pid", "1");
    const snapshotd::Message foreign_pid_response = client.Request(foreign_pid_request);
    Expect(foreign_pid_response.command == "error",
           "checkpoint-pid should reject processes owned by another uid");

    snapshotd::Message unsafe_pid_request;
    unsafe_pid_request.command = "checkpoint-pid";
    unsafe_pid_request.AddField("pid", std::to_string(job_pid));
    unsafe_pid_request.AddField("extra_arg", "--action-script=/tmp/evil");
    const snapshotd::Message unsafe_pid_response = client.Request(unsafe_pid_request);
    Expect(unsafe_pid_response.command == "error",
           "checkpoint-pid should reject unsafe CRIU args");

    snapshotd::Message pid_checkpoint_request;
    pid_checkpoint_request.command = "checkpoint-pid";
    pid_checkpoint_request.AddField("pid", std::to_string(job_pid));
    const snapshotd::Message pid_checkpoint_response = client.Request(pid_checkpoint_request);
    Expect(pid_checkpoint_response.command == "ok", "checkpoint-pid request failed");
    Expect(snapshotd::IsSafeId(pid_checkpoint_response.Get("job_id")),
           "checkpoint-pid should return a safe job id");
    Expect(snapshotd::IsSafeId(pid_checkpoint_response.Get("checkpoint_id")),
           "checkpoint-pid should return a safe checkpoint id");
    Expect(fs::is_directory(fs::path(pid_checkpoint_response.Get("export_images"))),
           "checkpoint-pid should export readable images");
    Expect(fs::exists(fs::path(pid_checkpoint_response.Get("dump_log"))),
           "checkpoint-pid should export a readable dump log");

    CommandResult checkpoint_result = RunCommand(
        {ctl_bin.string(), "--socket-path", socket_path.string(), "checkpoint", job_id});
    Expect(checkpoint_result.exit_code == 0, "checkpoint command failed");
    const auto state_perms = fs::status(state_dir).permissions();
    Expect((state_perms & fs::perms::owner_exec) != fs::perms::none,
           "state dir should remain searchable by owner");
    Expect((state_perms & fs::perms::group_exec) != fs::perms::none,
           "state dir should remain traversable for exported artifacts");
    Expect((state_perms & fs::perms::others_exec) != fs::perms::none,
           "state dir should remain traversable for exported artifacts");
    const auto checkpoint_map = ParseOutputMap(checkpoint_result.stdout_text);
    const std::string checkpoint_id = checkpoint_map.at("checkpoint_id");
    Expect(snapshotd::IsSafeId(checkpoint_id), "checkpoint id should be safe");

    const std::string call_log_text = snapshotd::ReadTextFile(call_log);
    Expect(call_log_text.find("arg=dump") != std::string::npos, "dump call missing");
    Expect(call_log_text.find("arg=--no-default-config") != std::string::npos,
           "worker must force --no-default-config");
    Expect(call_log_text.find("arg=--leave-running") != std::string::npos,
           "worker must force --leave-running");
    Expect(call_log_text.find("--action-script") == std::string::npos,
           "unsafe action script must not reach criu");

    const std::string env_log_text = snapshotd::ReadTextFile(env_log);
    Expect(env_log_text.find("CRIU_CONFIG_FILE=") == std::string::npos,
           "worker should clear CRIU_CONFIG_FILE");

    snapshotd::Message dropped_status_request;
    dropped_status_request.command = "status";
    dropped_status_request.AddField("job_id", job_id);
    SendAndClose(socket_path, dropped_status_request);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CommandResult status_after_disconnect =
        RunCommand({ctl_bin.string(), "--socket-path", socket_path.string(), "status", job_id});
    Expect(status_after_disconnect.exit_code == 0,
           "daemon should survive a client disconnect during response write");

    CommandResult restore_result =
        RunCommand({ctl_bin.string(), "--socket-path", socket_path.string(), "restore", job_id});
    Expect(restore_result.exit_code == 0, "restore command failed: " + restore_result.stderr_text);
    const auto restore_map = ParseOutputMap(restore_result.stdout_text);
    restored_pid = static_cast<pid_t>(std::stol(restore_map.at("restored_pid")));
    Expect(restored_pid > 1, "restore pidfile was not propagated");

    CommandResult status_after_restore =
        RunCommand({ctl_bin.string(), "--socket-path", socket_path.string(), "status", job_id});
    Expect(status_after_restore.exit_code == 0, "status after restore failed");
    const auto status_after_restore_map = ParseOutputMap(status_after_restore.stdout_text);
    Expect(status_after_restore_map.at("pid") == std::to_string(restored_pid),
           "managed job did not adopt restored pid");
    Expect(status_after_restore_map.at("state") == "running",
           "restored managed job should be running");

    CommandResult second_checkpoint =
        RunCommand({ctl_bin.string(), "--socket-path", socket_path.string(), "checkpoint", job_id});
    Expect(second_checkpoint.exit_code == 0, "checkpoint after restore failed");

    const std::string call_log_after_restore = snapshotd::ReadTextFile(call_log);
    Expect(call_log_after_restore.find("arg=restore") != std::string::npos,
           "restore call missing");
    Expect(call_log_after_restore.find("argv0=" + fake_criu_ns.string()) == std::string::npos,
           "default checkpoint/restore flow should not invoke criu-ns");
    Expect(call_log_after_restore.find("arg=" + std::to_string(restored_pid)) != std::string::npos,
           "post-restore checkpoint did not target restored pid");
  } catch (...) {
    if (restored_pid > 1) {
      kill(restored_pid, SIGTERM);
      waitpid(restored_pid, nullptr, 0);
    }
    if (job_pid > 1) {
      kill(job_pid, SIGTERM);
      waitpid(job_pid, nullptr, 0);
    }
    KillAndWait(daemon_pid);
    snapshotd::RemoveTree(temp_dir);
    throw;
  }

  if (restored_pid > 1) {
    kill(restored_pid, SIGTERM);
    waitpid(restored_pid, nullptr, 0);
  }
  if (job_pid > 1) {
    kill(job_pid, SIGTERM);
    waitpid(job_pid, nullptr, 0);
  }
  KillAndWait(daemon_pid);
  snapshotd::RemoveTree(temp_dir);
}

void TestWorkerTimeoutCancelsHungOperation() {
  const fs::path temp_dir = MakeTempDir("timeoutit");
  const fs::path socket_path = temp_dir / "snapshotd.sock";
  const fs::path state_dir = temp_dir / "state";
  const fs::path ready_file = temp_dir / "ready";
  const fs::path fake_criu = temp_dir / "fake-criu-timeout.sh";
  const fs::path fake_criu_ns = temp_dir / "criu-ns";
  const fs::path daemon_bin = Runfile("src/csrc/snapshotd");
  const fs::path worker_bin = Runfile("src/csrc/snapshot-worker");
  const fs::path ctl_bin = Runfile("src/csrc/snapshotctl");

  WriteExecutable(
      fake_criu,
      "#!/usr/bin/env bash\n"
      "set -eu\n"
      "mode=\"${1:-}\"\n"
      "if [ \"$mode\" = 'dump' ]; then\n"
      "  sleep 5\n"
      "fi\n"
      "logfile=''\n"
      "while [ \"$#\" -gt 0 ]; do\n"
      "  if [ \"$1\" = '--log-file' ]; then\n"
      "    logfile=\"$2\"\n"
      "    shift 2\n"
      "    continue\n"
      "  fi\n"
      "  shift\n"
      "done\n"
      "if [ -n \"$logfile\" ]; then\n"
      "  mkdir -p \"$(dirname \"$logfile\")\"\n"
      "  printf '%s\\n' 'timeout log' > \"$logfile\"\n"
      "fi\n");
  WriteExecutable(fake_criu_ns, snapshotd::ReadTextFile(fake_criu));

  const pid_t daemon_pid = SpawnDaemon(
      daemon_bin,
      socket_path,
      state_dir,
      worker_bin,
      fake_criu,
      fake_criu_ns,
      ready_file,
      {},
      1);
  pid_t job_pid = 0;
  try {
    WaitForPath(ready_file, std::chrono::milliseconds(5000));
    WaitForPath(socket_path, std::chrono::milliseconds(5000));

    CommandResult run_result = RunCommand(
        {ctl_bin.string(), "--socket-path", socket_path.string(), "run", "--", "/bin/sleep", "30"});
    Expect(run_result.exit_code == 0, "run command failed");
    const auto run_map = ParseOutputMap(run_result.stdout_text);
    const std::string job_id = run_map.at("job_id");
    job_pid = static_cast<pid_t>(std::stol(run_map.at("pid")));

    CommandResult checkpoint_result =
        RunCommand({ctl_bin.string(), "--socket-path", socket_path.string(), "checkpoint", job_id});
    Expect(checkpoint_result.exit_code != 0, "hung checkpoint should fail after timeout");
    Expect(
        checkpoint_result.stderr_text.find("timed out") != std::string::npos,
        "checkpoint timeout should be reported to the caller");

    CommandResult status_result =
        RunCommand({ctl_bin.string(), "--socket-path", socket_path.string(), "status", job_id});
    Expect(status_result.exit_code == 0, "daemon should remain responsive after worker timeout");
  } catch (...) {
    if (job_pid > 1) {
      kill(job_pid, SIGTERM);
      waitpid(job_pid, nullptr, 0);
    }
    KillAndWait(daemon_pid);
    snapshotd::RemoveTree(temp_dir);
    throw;
  }

  if (job_pid > 1) {
    kill(job_pid, SIGTERM);
    waitpid(job_pid, nullptr, 0);
  }
  KillAndWait(daemon_pid);
  snapshotd::RemoveTree(temp_dir);
}

}  // namespace

int main() {
  try {
    TestDaemonFlowAndMaliciousInputs();
    TestWorkerTimeoutCancelsHungOperation();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
