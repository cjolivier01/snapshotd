/** @file
 *  @brief Shared helpers for filesystem, process, and string handling.
 */

#ifndef SNAPSHOT_CSRC_UTIL_H_
#define SNAPSHOT_CSRC_UTIL_H_

#include <sys/types.h>

#include <filesystem>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace snapshotd {

/** @brief Stable process identity token used to defend against PID reuse. */
struct ProcessIdentity {
  pid_t pid = 0;
  uid_t uid = 0;
  gid_t gid = 0;
  std::string start_time_ticks;
};

/** @brief runtime_error that preserves the originating errno value. */
class ErrnoRuntimeError : public std::runtime_error {
 public:
  ErrnoRuntimeError(std::string message, int error_code);

  int error_code() const { return error_code_; }

 private:
  int error_code_ = 0;
};

/** @brief Return `context: strerror(errno)` using the current errno value. */
std::string ErrnoMessage(const std::string& context);
/** @brief Throw a runtime_error built from the current errno value. */
[[noreturn]] void ThrowErrno(const std::string& context);

/** @brief Read a full text file into memory. */
std::string ReadTextFile(const std::filesystem::path& path);
/** @brief Write a text file, creating parent directories as needed. */
void WriteTextFile(
    const std::filesystem::path& path,
    const std::string& text,
    mode_t file_mode = 0644,
    mode_t parent_mode = 0755);
/** @brief Ensure a directory exists with at least the requested owner mode. */
void EnsureDir(const std::filesystem::path& path, mode_t mode = 0755);
/** @brief Remove a directory tree if it exists. */
void RemoveTree(const std::filesystem::path& path);

/** @brief Return an environment variable or a default fallback. */
std::string GetEnv(const std::string& name, const std::string& default_value = "");
/** @brief Return the current working directory as an absolute path string. */
std::string GetCurrentWorkingDirectory();

/** @brief Return true if the value is safe to use as an on-disk identifier. */
bool IsSafeId(const std::string& value);
/** @brief Throw if the identifier contains path separators or traversal characters. */
void RequireSafeId(const std::string& value, const std::string& field_name);
/** @brief Generate a random identifier with a stable prefix. */
std::string GenerateId(const std::string& prefix);

/** @brief Return true when the path string is absolute. */
bool IsAbsolutePath(const std::string& path);
/** @brief Resolve an executable against PATH, returning an absolute path. */
std::string ResolveExecutable(const std::string& executable, const std::string& path_env);
/** @brief Return true if a filesystem entry exists. */
bool PathExists(const std::filesystem::path& path);
/** @brief Return true if @p candidate lives underneath @p root after normalization. */
bool IsPathBeneath(const std::filesystem::path& root, const std::filesystem::path& candidate);

/** @brief Parse `key=value` text into a sorted map. */
std::map<std::string, std::string> ParseKeyValueText(const std::string& text);
/** @brief Serialize a key/value map into newline-delimited `key=value` text. */
std::string SerializeKeyValueMap(const std::map<std::string, std::string>& values);

/** @brief Read and normalize a symlink target. */
std::string ReadSymlinkPath(const std::filesystem::path& path);
/** @brief Join argv tokens into a shell-style debug string. */
std::string JoinCommandLine(const std::vector<std::string>& argv);
/** @brief Return true if `kill(pid, 0)` reports the process still exists. */
bool IsProcessAlive(pid_t pid);
/** @brief Read the kernel start-time tick field for a process. */
std::string ReadProcessStartTimeTicks(pid_t pid);
/** @brief Read the real UID that owns the process. */
uid_t ReadProcessRealUid(pid_t pid);
/** @brief Read the real GID that owns the process. */
gid_t ReadProcessRealGid(pid_t pid);
/** @brief Resolve `/proc/<pid>/exe`. */
std::string ReadProcessExecutablePath(pid_t pid);
/** @brief Resolve `/proc/<pid>/cwd`. */
std::string ReadProcessWorkingDirectory(pid_t pid);
/** @brief Read `/proc/<pid>/cmdline` into argv tokens. */
std::vector<std::string> ReadProcessCommandLine(pid_t pid);
/** @brief Read the process fields needed to guard against PID reuse. */
ProcessIdentity ReadProcessIdentity(pid_t pid);
/**
 * @brief Return true only if the pid still belongs to the expected real
 * uid/gid and start-time tuple.
 *
 * This is a PID-reuse check, not a full privilege-state check.
 */
bool ProcessIdentityMatches(
    pid_t pid,
    uid_t expected_uid,
    gid_t expected_gid,
    const std::string& expected_start_time_ticks);
/**
 * @brief Return true only if the process still matches the expected start time
 * and has no elevated uid/gid/capability state relative to the caller.
 */
bool ProcessMatchesPeerSecurity(
    pid_t pid,
    uid_t expected_uid,
    gid_t expected_gid,
    const std::string& expected_start_time_ticks,
    std::string* reason = nullptr);
/**
 * @brief Reject managed-job executables that would grant extra privilege on
 * exec, such as setuid/setgid or file-capability binaries.
 */
void ValidateManagedExecutable(const std::string& path);
/**
 * @brief Open and validate the exact managed-job executable that will be
 * executed, returning an fd suitable for execveat().
 */
int OpenManagedExecutableForExec(const std::string& path);
/** @brief Convert uid/gid/pid values into decimal strings. */
std::string UidToString(uid_t uid);
std::string GidToString(gid_t gid);
std::string PidToString(pid_t pid);
/** @brief Recursively copy a directory tree. */
void CopyTree(const std::filesystem::path& source, const std::filesystem::path& destination);
/** @brief Recursively chown a directory tree. */
void ChownTree(const std::filesystem::path& root, uid_t uid, gid_t gid);
/** @brief Recursively apply distinct directory/file modes to a tree. */
void SetTreePermissions(
    const std::filesystem::path& root,
    mode_t directory_mode,
    mode_t file_mode);
/** @brief Return the total size in bytes of all regular files under a tree. */
std::uint64_t DirectoryTreeSizeBytes(const std::filesystem::path& root);

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_UTIL_H_
