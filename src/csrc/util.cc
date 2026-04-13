/** @file
 *  @brief Shared filesystem, process, and string helpers for snapshotd.
 *
 *  @details
 *  Several helpers in this file are security-sensitive rather than merely
 *  convenient: process identity checks guard against PID reuse, executable
 *  validation blocks setuid/setgid and file-capability launch paths, and path
 *  utilities help keep worker state confined to broker-owned directories.
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
#include <sys/xattr.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace snapshotd {

namespace fs = std::filesystem;

namespace {

/** @brief Parsed identity and capability fields extracted from `/proc/<pid>/status`. */
struct ProcessStatusFields {
  std::array<uid_t, 4> uids {};
  std::array<gid_t, 4> gids {};
  std::string cap_inh;
  std::string cap_prm;
  std::string cap_eff;
  std::string cap_amb;
  bool have_uids = false;
  bool have_gids = false;
  bool have_cap_inh = false;
  bool have_cap_prm = false;
  bool have_cap_eff = false;
  bool have_cap_amb = false;
};

/**
 * @brief Ensure a directory exists, creating parents recursively as needed.
 *
 * @param path Directory path to create or validate.
 * @param mode Mode applied to newly created directories.
 * @param chmod_existing Whether an existing directory should be chmod'd too.
 */
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

/** @brief Write a full string to an fd, retrying on `EINTR`. */
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

/** @brief Parse four numeric ids from one `/proc` status line payload. */
bool ParseFourIds(const std::string& payload, std::array<unsigned long long, 4>* values) {
  std::istringstream input(payload);
  for (std::size_t index = 0; index < values->size(); ++index) {
    if (!(input >> (*values)[index])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Read uid/gid tuples and capability masks from `/proc/<pid>/status`.
 *
 * These fields are used to reject processes whose security context no longer
 * matches the requesting peer.
 */
ProcessStatusFields ReadProcessStatusFields(pid_t pid) {
  const fs::path status_path = fs::path("/proc") / PidToString(pid) / "status";
  std::istringstream lines(ReadTextFile(status_path));
  ProcessStatusFields fields;
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("Uid:", 0) == 0) {
      std::array<unsigned long long, 4> values {};
      if (!ParseFourIds(line.substr(4), &values)) {
        throw std::runtime_error("failed to parse Uid line from " + status_path.string());
      }
      for (std::size_t index = 0; index < values.size(); ++index) {
        fields.uids[index] = static_cast<uid_t>(values[index]);
      }
      fields.have_uids = true;
      continue;
    }
    if (line.rfind("Gid:", 0) == 0) {
      std::array<unsigned long long, 4> values {};
      if (!ParseFourIds(line.substr(4), &values)) {
        throw std::runtime_error("failed to parse Gid line from " + status_path.string());
      }
      for (std::size_t index = 0; index < values.size(); ++index) {
        fields.gids[index] = static_cast<gid_t>(values[index]);
      }
      fields.have_gids = true;
      continue;
    }
    if (line.rfind("CapInh:", 0) == 0) {
      fields.cap_inh = line.substr(7);
      fields.have_cap_inh = true;
      continue;
    }
    if (line.rfind("CapPrm:", 0) == 0) {
      fields.cap_prm = line.substr(7);
      fields.have_cap_prm = true;
      continue;
    }
    if (line.rfind("CapEff:", 0) == 0) {
      fields.cap_eff = line.substr(7);
      fields.have_cap_eff = true;
      continue;
    }
    if (line.rfind("CapAmb:", 0) == 0) {
      fields.cap_amb = line.substr(7);
      fields.have_cap_amb = true;
      continue;
    }
  }

  if (!fields.have_uids || !fields.have_gids || !fields.have_cap_inh ||
      !fields.have_cap_prm || !fields.have_cap_eff || !fields.have_cap_amb) {
    throw std::runtime_error("failed to parse process status fields from " + status_path.string());
  }
  return fields;
}

/** @brief Return true when a capability bitmask contains only zero digits. */
bool IsZeroCapabilityMask(const std::string& value) {
  bool saw_hex_digit = false;
  for (char current : value) {
    if (std::isspace(static_cast<unsigned char>(current))) {
      continue;
    }
    saw_hex_digit = true;
    if (current != '0') {
      return false;
    }
  }
  return saw_hex_digit;
}

/** @brief Verify that all saved, effective, and fs UIDs match the expected uid. */
bool MatchesExpectedUidTuple(
    const std::array<uid_t, 4>& values,
    uid_t expected_uid) {
  for (uid_t current : values) {
    if (current != expected_uid) {
      return false;
    }
  }
  return true;
}

/** @brief Verify that all saved, effective, and fs GIDs match the expected gid. */
bool MatchesExpectedGidTuple(
    const std::array<gid_t, 4>& values,
    gid_t expected_gid) {
  for (gid_t current : values) {
    if (current != expected_gid) {
      return false;
    }
  }
  return true;
}

/** @brief Build a stable `/proc/self/fd/<n>` path for xattr and reopen checks. */
std::string ProcFdPath(int fd) {
  return (fs::path("/proc/self/fd") / std::to_string(fd)).string();
}

/**
 * @brief Reject non-regular, setuid, setgid, or file-capability executables.
 *
 * Managed jobs must not begin from binaries that can acquire privilege through
 * file metadata after the daemon drops to the caller's uid/gid.
 */
void ValidateManagedExecutableFd(int fd, const std::string& path) {
  struct stat executable_stat {};
  if (fstat(fd, &executable_stat) != 0) {
    ThrowErrno("fstat(" + path + ")");
  }
  if (!S_ISREG(executable_stat.st_mode)) {
    throw std::runtime_error("managed job executable must be a regular file");
  }
  if ((executable_stat.st_mode & S_ISUID) != 0 || (executable_stat.st_mode & S_ISGID) != 0) {
    throw std::runtime_error("managed job executable may not be setuid or setgid");
  }

  errno = 0;
  const ssize_t capability_size =
      getxattr(ProcFdPath(fd).c_str(), "security.capability", nullptr, 0);
  if (capability_size >= 0) {
    throw std::runtime_error("managed job executable may not have file capabilities");
  }
  if (errno == ENODATA || errno == ENOTSUP
#ifdef ENOATTR
      || errno == ENOATTR
#endif
  ) {
    return;
  }
  ThrowErrno("getxattr(" + path + ", security.capability)");
}

/** @brief Detect shebang scripts so exec helpers can preserve the executable fd. */
bool ExecutableFdLooksLikeScript(int fd) {
  // Shebang scripts need the executable fd to survive execveat() because the
  // kernel passes the script to the interpreter via /dev/fd/N.
  const int readable_fd = open(ProcFdPath(fd).c_str(), O_RDONLY | O_CLOEXEC);
  if (readable_fd < 0) {
    if (errno == EACCES || errno == EPERM) {
      return false;
    }
    ThrowErrno("open(" + ProcFdPath(fd) + ")");
  }

  char header[2] = {'\0', '\0'};
  ssize_t count = 0;
  while (count < static_cast<ssize_t>(sizeof(header))) {
    const ssize_t current =
        read(readable_fd, header + count, sizeof(header) - static_cast<std::size_t>(count));
    if (current < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int saved_errno = errno;
      close(readable_fd);
      errno = saved_errno;
      ThrowErrno("read(" + ProcFdPath(fd) + ")");
    }
    if (current == 0) {
      break;
    }
    count += current;
  }
  if (close(readable_fd) != 0) {
    ThrowErrno("close(" + ProcFdPath(fd) + ")");
  }
  return count == static_cast<ssize_t>(sizeof(header)) &&
         header[0] == '#' &&
         header[1] == '!';
}

}  // namespace

/** @brief Format the current `errno` value with context text. */
std::string ErrnoMessage(const std::string& context) {
  return context + ": " + std::string(strerror(errno));
}

ErrnoRuntimeError::ErrnoRuntimeError(std::string message, int error_code)
    : std::runtime_error(std::move(message)), error_code_(error_code) {}

[[noreturn]] void ThrowErrno(const std::string& context) {
  const int error_code = errno;
  throw ErrnoRuntimeError(
      context + ": " + std::string(strerror(error_code)),
      error_code);
}

/** @brief Read an entire text file into memory. */
std::string ReadTextFile(const fs::path& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    throw std::runtime_error("failed to open file for read: " + path.string());
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

/**
 * @brief Write a text file atomically enough for repo metadata use.
 *
 * Parent directories are created first with @p parent_mode so private state
 * does not need to rely on the caller's umask.
 */
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

/** @brief Recursively remove one filesystem subtree. */
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

/** @brief Return the current working directory as an absolute path string. */
std::string GetCurrentWorkingDirectory() {
  std::vector<char> buffer(PATH_MAX, '\0');
  if (getcwd(buffer.data(), buffer.size()) == nullptr) {
    ThrowErrno("getcwd");
  }
  return std::string(buffer.data());
}

/** @brief Validate the restricted identifier format used for job and checkpoint ids. */
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

/** @brief Throw when a job or checkpoint identifier violates the safe-id rules. */
void RequireSafeId(const std::string& value, const std::string& field_name) {
  if (!IsSafeId(value)) {
    throw std::runtime_error("unsafe " + field_name + ": " + value);
  }
}

/** @brief Generate a short broker-owned identifier with a fixed safe prefix. */
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

/** @brief Return true when a path string is syntactically absolute. */
bool IsAbsolutePath(const std::string& path) {
  return !path.empty() && path[0] == '/';
}

/**
 * @brief Resolve an executable path the same way the CLI does before brokering.
 *
 * Absolute paths are canonicalized directly; bare command names are resolved
 * against the supplied PATH string.
 */
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

/** @brief Best-effort existence check that suppresses filesystem exceptions. */
bool PathExists(const fs::path& path) {
  std::error_code error;
  return fs::exists(path, error);
}

/** @brief Return true when @p candidate stays under @p root after canonicalization. */
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

/** @brief Parse `key=value` lines into a simple map. */
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

/** @brief Serialize a map to deterministic newline-delimited `key=value` text. */
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

/** @brief Read one symlink target, usually from `/proc`. */
std::string ReadSymlinkPath(const fs::path& path) {
  std::vector<char> buffer(PATH_MAX + 1, '\0');
  const ssize_t count = readlink(path.c_str(), buffer.data(), buffer.size() - 1);
  if (count < 0) {
    ThrowErrno("readlink(" + path.string() + ")");
  }
  buffer[static_cast<std::size_t>(count)] = '\0';
  return std::string(buffer.data());
}

/** @brief Render an argv vector as a shell-safe single-line string. */
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
  return ReadProcessStatusFields(pid).uids[0];
}

gid_t ReadProcessRealGid(pid_t pid) {
  return ReadProcessStatusFields(pid).gids[0];
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
  identity.gid = ReadProcessRealGid(pid);
  identity.start_time_ticks = ReadProcessStartTimeTicks(pid);
  return identity;
}

bool ProcessIdentityMatches(
    pid_t pid,
    uid_t expected_uid,
    gid_t expected_gid,
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
           identity.gid == expected_gid &&
           identity.start_time_ticks == expected_start_time_ticks;
  } catch (...) {
    return false;
  }
}

bool ProcessMatchesPeerSecurity(
    pid_t pid,
    uid_t expected_uid,
    gid_t expected_gid,
    const std::string& expected_start_time_ticks,
    std::string* reason) {
  auto set_reason = [&](const std::string& value) {
    if (reason != nullptr) {
      *reason = value;
    }
  };

  if (pid <= 0 || expected_start_time_ticks.empty()) {
    set_reason("invalid process identity");
    return false;
  }
  try {
    if (!IsProcessAlive(pid)) {
      set_reason("process is not running");
      return false;
    }
    const ProcessIdentity identity = ReadProcessIdentity(pid);
    if (identity.start_time_ticks != expected_start_time_ticks) {
      set_reason("process start time changed");
      return false;
    }
    const ProcessStatusFields status = ReadProcessStatusFields(pid);
    if (!MatchesExpectedUidTuple(status.uids, expected_uid)) {
      set_reason("process uid tuple is elevated or no longer owned by the caller");
      return false;
    }
    if (!MatchesExpectedGidTuple(status.gids, expected_gid)) {
      set_reason("process gid tuple is elevated or no longer owned by the caller");
      return false;
    }
    if (!IsZeroCapabilityMask(status.cap_inh) || !IsZeroCapabilityMask(status.cap_prm) ||
        !IsZeroCapabilityMask(status.cap_eff) || !IsZeroCapabilityMask(status.cap_amb)) {
      set_reason("process holds Linux capabilities");
      return false;
    }
    return true;
  } catch (const std::exception& error) {
    set_reason(error.what());
    return false;
  }
}

void ValidateManagedExecutable(const std::string& path) {
  const int executable_fd = OpenManagedExecutableForExec(path);
  if (close(executable_fd) != 0) {
    ThrowErrno("close(" + path + ")");
  }
}

int OpenManagedExecutableForExec(const std::string& path) {
  if (!IsAbsolutePath(path)) {
    throw std::runtime_error("managed job executable must be absolute");
  }

  const int executable_fd = open(path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
  if (executable_fd < 0) {
    ThrowErrno("open(" + path + ")");
  }
  try {
    ValidateManagedExecutableFd(executable_fd, path);
    if (ExecutableFdLooksLikeScript(executable_fd)) {
      const int flags = fcntl(executable_fd, F_GETFD);
      if (flags < 0) {
        ThrowErrno("fcntl(F_GETFD)");
      }
      if (fcntl(executable_fd, F_SETFD, flags & ~FD_CLOEXEC) != 0) {
        ThrowErrno("fcntl(F_SETFD)");
      }
    }
  } catch (...) {
    close(executable_fd);
    throw;
  }
  return executable_fd;
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

void SetTreePermissions(
    const fs::path& root,
    mode_t directory_mode,
    mode_t file_mode) {
  if (!PathExists(root)) {
    return;
  }
  auto chmod_one = [&](const fs::path& path, mode_t mode) {
    if (chmod(path.c_str(), mode) != 0) {
      ThrowErrno("chmod(" + path.string() + ")");
    }
  };
  chmod_one(root, directory_mode);
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    const mode_t mode = entry.is_directory() ? directory_mode : file_mode;
    chmod_one(entry.path(), mode);
  }
}

std::uint64_t DirectoryTreeSizeBytes(const fs::path& root) {
  if (!PathExists(root)) {
    return 0;
  }
  std::uint64_t total = 0;
  std::error_code error;
  fs::recursive_directory_iterator iterator(
      root,
      fs::directory_options::skip_permission_denied,
      error);
  if (error) {
    throw std::runtime_error("failed to walk tree " + root.string() + ": " + error.message());
  }
  for (const auto& entry : iterator) {
    std::error_code status_error;
    if (!entry.is_regular_file(status_error) || status_error) {
      continue;
    }
    const auto file_size = entry.file_size(status_error);
    if (status_error) {
      continue;
    }
    total += static_cast<std::uint64_t>(file_size);
  }
  return total;
}

}  // namespace snapshotd
