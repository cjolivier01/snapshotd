/** @file
 *  @brief Shared filesystem, process, and string helpers for snapshotd.
 */

#include "src/csrc/util.h"

#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace snapshotd {

namespace fs = std::filesystem;

namespace {

void EnsureDirImpl(const fs::path& path, mode_t mode, bool chmod_existing) {
  if (path.empty()) {
    return;
  }

  std::error_code error;
  if (fs::exists(path, error)) {
    if (error) {
      throw std::runtime_error("failed to stat directory " + path.string() + ": " +
                               error.message());
    }
    if (!fs::is_directory(path, error)) {
      throw std::runtime_error("path exists but is not a directory: " + path.string());
    }
    if (error) {
      throw std::runtime_error("failed to stat directory " + path.string() + ": " +
                               error.message());
    }
    if (chmod_existing && chmod(path.c_str(), mode) != 0 && errno != ENOENT) {
      ThrowErrno("chmod(" + path.string() + ")");
    }
    return;
  }

  EnsureDirImpl(path.parent_path(), mode, false);
  if (mkdir(path.c_str(), mode) != 0) {
    if (errno != EEXIST) {
      ThrowErrno("mkdir(" + path.string() + ")");
    }
    return;
  }
  if (chmod(path.c_str(), mode) != 0 && errno != ENOENT) {
    ThrowErrno("chmod(" + path.string() + ")");
  }
}

void WriteAll(int fd, const std::string& text) {
  const char* cursor = text.data();
  std::size_t remaining = text.size();
  while (remaining > 0) {
    const ssize_t written = write(fd, cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno("write");
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
}

}  // namespace

std::string ErrnoMessage(const std::string& context) {
  return context + ": " + std::string(strerror(errno));
}

[[noreturn]] void ThrowErrno(const std::string& context) {
  throw std::runtime_error(ErrnoMessage(context));
}

std::string ReadTextFile(const fs::path& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    throw std::runtime_error("failed to open file for read: " + path.string());
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

void WriteTextFile(
    const fs::path& path,
    const std::string& text,
    mode_t file_mode,
    mode_t parent_mode) {
  EnsureDirImpl(path.parent_path(), parent_mode, false);
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, file_mode);
  if (fd < 0) {
    ThrowErrno("open(" + path.string() + ")");
  }
  try {
    WriteAll(fd, text);
    if (fchmod(fd, file_mode) != 0) {
      ThrowErrno("fchmod(" + path.string() + ")");
    }
  } catch (...) {
    close(fd);
    throw;
  }
  if (close(fd) != 0) {
    ThrowErrno("close(" + path.string() + ")");
  }
}

void EnsureDir(const fs::path& path, mode_t mode) {
  EnsureDirImpl(path, mode, true);
}

void RemoveTree(const fs::path& path) {
  std::error_code error;
  fs::remove_all(path, error);
  if (error) {
    throw std::runtime_error("failed to remove tree " + path.string() + ": " +
                             error.message());
  }
}

std::string GetEnv(const std::string& name, const std::string& default_value) {
  const char* value = getenv(name.c_str());
  if (value == nullptr) {
    return default_value;
  }
  return std::string(value);
}

std::string GetCurrentWorkingDirectory() {
  std::vector<char> buffer(PATH_MAX, '\0');
  if (getcwd(buffer.data(), buffer.size()) == nullptr) {
    ThrowErrno("getcwd");
  }
  return std::string(buffer.data());
}

bool IsSafeId(const std::string& value) {
  if (value.empty() || value.size() > 80) {
    return false;
  }
  if (!std::isalnum(static_cast<unsigned char>(value[0]))) {
    return false;
  }
  for (char current : value) {
    const unsigned char byte = static_cast<unsigned char>(current);
    if (!(std::isalnum(byte) || current == '-')) {
      return false;
    }
  }
  return true;
}

void RequireSafeId(const std::string& value, const std::string& field_name) {
  if (!IsSafeId(value)) {
    throw std::runtime_error("unsafe " + field_name + ": " + value);
  }
}

std::string GenerateId(const std::string& prefix) {
  RequireSafeId(prefix, "prefix");
  std::random_device device;
  std::ostringstream stream;
  stream << prefix << "-";
  for (int index = 0; index < 8; ++index) {
    const unsigned value = device() & 0xffU;
    stream << std::hex << std::setw(2) << std::setfill('0') << value;
  }
  return stream.str();
}

bool IsAbsolutePath(const std::string& path) {
  return !path.empty() && path[0] == '/';
}

std::string ResolveExecutable(const std::string& executable, const std::string& path_env) {
  if (executable.empty()) {
    throw std::runtime_error("missing executable");
  }
  fs::path candidate(executable);
  if (candidate.is_absolute()) {
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(candidate, error);
    if (error) {
      throw std::runtime_error("failed to resolve executable " + executable + ": " +
                               error.message());
    }
    return canonical.string();
  }
  if (executable.find('/') != std::string::npos) {
    fs::path joined = fs::absolute(candidate);
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(joined, error);
    if (error) {
      throw std::runtime_error("failed to resolve executable " + executable + ": " +
                               error.message());
    }
    return canonical.string();
  }

  std::stringstream paths(path_env);
  std::string entry;
  while (std::getline(paths, entry, ':')) {
    if (entry.empty()) {
      continue;
    }
    fs::path path = fs::path(entry) / executable;
    if (access(path.c_str(), X_OK) == 0) {
      std::error_code error;
      const fs::path canonical = fs::weakly_canonical(path, error);
      if (!error) {
        return canonical.string();
      }
    }
  }
  throw std::runtime_error("could not resolve executable on PATH: " + executable);
}

bool PathExists(const fs::path& path) {
  std::error_code error;
  return fs::exists(path, error);
}

bool IsPathBeneath(const fs::path& root, const fs::path& candidate) {
  std::error_code root_error;
  std::error_code candidate_error;
  const fs::path canonical_root = fs::weakly_canonical(root, root_error);
  const fs::path canonical_candidate = fs::weakly_canonical(candidate, candidate_error);
  if (root_error || candidate_error) {
    return false;
  }
  auto root_it = canonical_root.begin();
  auto candidate_it = canonical_candidate.begin();
  while (root_it != canonical_root.end() && candidate_it != canonical_candidate.end()) {
    if (*root_it != *candidate_it) {
      return false;
    }
    ++root_it;
    ++candidate_it;
  }
  return root_it == canonical_root.end();
}

std::map<std::string, std::string> ParseKeyValueText(const std::string& text) {
  std::map<std::string, std::string> values;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty()) {
      continue;
    }
    const std::size_t delimiter = line.find('=');
    if (delimiter == std::string::npos) {
      throw std::runtime_error("invalid key-value line: " + line);
    }
    values[line.substr(0, delimiter)] = line.substr(delimiter + 1);
  }
  return values;
}

std::string SerializeKeyValueMap(const std::map<std::string, std::string>& values) {
  std::ostringstream output;
  for (const auto& [key, value] : values) {
    if (key.find('=') != std::string::npos || key.find('\n') != std::string::npos ||
        value.find('\n') != std::string::npos) {
      throw std::runtime_error("invalid key/value content");
    }
    output << key << "=" << value << "\n";
  }
  return output.str();
}

std::string ReadSymlinkPath(const fs::path& path) {
  std::vector<char> buffer(PATH_MAX + 1, '\0');
  const ssize_t count = readlink(path.c_str(), buffer.data(), buffer.size() - 1);
  if (count < 0) {
    ThrowErrno("readlink(" + path.string() + ")");
  }
  buffer[static_cast<std::size_t>(count)] = '\0';
  return std::string(buffer.data());
}

std::string JoinCommandLine(const std::vector<std::string>& argv) {
  std::ostringstream output;
  bool first = true;
  for (const std::string& token : argv) {
    if (!first) {
      output << " ";
    }
    first = false;
    if (token.find_first_of(" \t\n'\"\\") == std::string::npos) {
      output << token;
      continue;
    }
    output << "'";
    for (char current : token) {
      if (current == '\'') {
        output << "'\\''";
      } else {
        output << current;
      }
    }
    output << "'";
  }
  return output.str();
}

bool IsProcessAlive(pid_t pid) {
  if (pid <= 0) {
    return false;
  }
  if (kill(pid, 0) == 0) {
    return true;
  }
  return errno == EPERM;
}

std::string ReadProcessStartTimeTicks(pid_t pid) {
  const fs::path stat_path = fs::path("/proc") / PidToString(pid) / "stat";
  const std::string text = ReadTextFile(stat_path);
  const std::size_t close_paren = text.rfind(')');
  if (close_paren == std::string::npos) {
    throw std::runtime_error("failed to parse " + stat_path.string());
  }
  std::istringstream fields(text.substr(close_paren + 2));
  std::string field;
  for (int index = 1; index <= 20; ++index) {
    if (!(fields >> field)) {
      throw std::runtime_error("failed to parse process start time from " + stat_path.string());
    }
  }
  return field;
}

uid_t ReadProcessRealUid(pid_t pid) {
  const fs::path status_path = fs::path("/proc") / PidToString(pid) / "status";
  std::istringstream lines(ReadTextFile(status_path));
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("Uid:", 0) != 0) {
      continue;
    }
    std::istringstream fields(line.substr(4));
    uid_t uid = 0;
    if (!(fields >> uid)) {
      break;
    }
    return uid;
  }
  throw std::runtime_error("failed to read process uid from " + status_path.string());
}

std::string ReadProcessExecutablePath(pid_t pid) {
  return ReadSymlinkPath(fs::path("/proc") / PidToString(pid) / "exe");
}

std::string ReadProcessWorkingDirectory(pid_t pid) {
  return ReadSymlinkPath(fs::path("/proc") / PidToString(pid) / "cwd");
}

std::vector<std::string> ReadProcessCommandLine(pid_t pid) {
  const fs::path cmdline_path = fs::path("/proc") / PidToString(pid) / "cmdline";
  std::ifstream input(cmdline_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("failed to open file for read: " + cmdline_path.string());
  }
  std::string payload(
      (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  std::vector<std::string> argv;
  std::string current;
  for (char byte : payload) {
    if (byte == '\0') {
      if (!current.empty()) {
        argv.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(byte);
  }
  if (!current.empty()) {
    argv.push_back(current);
  }
  return argv;
}

ProcessIdentity ReadProcessIdentity(pid_t pid) {
  ProcessIdentity identity;
  identity.pid = pid;
  identity.uid = ReadProcessRealUid(pid);
  identity.start_time_ticks = ReadProcessStartTimeTicks(pid);
  return identity;
}

bool ProcessIdentityMatches(
    pid_t pid,
    uid_t expected_uid,
    const std::string& expected_start_time_ticks) {
  if (pid <= 0 || expected_start_time_ticks.empty()) {
    return false;
  }
  try {
    if (!IsProcessAlive(pid)) {
      return false;
    }
    const ProcessIdentity identity = ReadProcessIdentity(pid);
    return identity.uid == expected_uid &&
           identity.start_time_ticks == expected_start_time_ticks;
  } catch (...) {
    return false;
  }
}

std::string UidToString(uid_t uid) {
  return std::to_string(static_cast<unsigned long long>(uid));
}

std::string GidToString(gid_t gid) {
  return std::to_string(static_cast<unsigned long long>(gid));
}

std::string PidToString(pid_t pid) {
  return std::to_string(static_cast<long long>(pid));
}

void CopyTree(const fs::path& source, const fs::path& destination) {
  RemoveTree(destination);
  if (!PathExists(source)) {
    return;
  }
  EnsureDir(destination.parent_path(), 0755);
  std::error_code error;
  fs::copy(
      source,
      destination,
      fs::copy_options::recursive | fs::copy_options::overwrite_existing,
      error);
  if (error) {
    throw std::runtime_error(
        "failed to copy tree " + source.string() + " -> " + destination.string() + ": " +
        error.message());
  }
}

void ChownTree(const fs::path& root, uid_t uid, gid_t gid) {
  if (!PathExists(root)) {
    return;
  }
  const auto chown_one = [&](const fs::path& path) {
    if (chown(path.c_str(), uid, gid) != 0) {
      ThrowErrno("chown(" + path.string() + ")");
    }
  };
  chown_one(root);
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    chown_one(entry.path());
  }
}

}  // namespace snapshotd
