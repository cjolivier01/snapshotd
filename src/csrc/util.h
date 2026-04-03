#ifndef SNAPSHOT_CSRC_UTIL_H_
#define SNAPSHOT_CSRC_UTIL_H_

#include <sys/types.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace snapshotd {

struct ProcessIdentity {
  pid_t pid = 0;
  uid_t uid = 0;
  std::string start_time_ticks;
};

std::string ErrnoMessage(const std::string& context);
[[noreturn]] void ThrowErrno(const std::string& context);

std::string ReadTextFile(const std::filesystem::path& path);
void WriteTextFile(
    const std::filesystem::path& path,
    const std::string& text,
    mode_t file_mode = 0644,
    mode_t parent_mode = 0755);
void EnsureDir(const std::filesystem::path& path, mode_t mode = 0755);
void RemoveTree(const std::filesystem::path& path);

std::string GetEnv(const std::string& name, const std::string& default_value = "");
std::string GetCurrentWorkingDirectory();

bool IsSafeId(const std::string& value);
void RequireSafeId(const std::string& value, const std::string& field_name);
std::string GenerateId(const std::string& prefix);

bool IsAbsolutePath(const std::string& path);
std::string ResolveExecutable(const std::string& executable, const std::string& path_env);
bool PathExists(const std::filesystem::path& path);
bool IsPathBeneath(const std::filesystem::path& root, const std::filesystem::path& candidate);

std::map<std::string, std::string> ParseKeyValueText(const std::string& text);
std::string SerializeKeyValueMap(const std::map<std::string, std::string>& values);

std::string ReadSymlinkPath(const std::filesystem::path& path);
std::string JoinCommandLine(const std::vector<std::string>& argv);
bool IsProcessAlive(pid_t pid);
std::string ReadProcessStartTimeTicks(pid_t pid);
uid_t ReadProcessRealUid(pid_t pid);
std::string ReadProcessExecutablePath(pid_t pid);
std::string ReadProcessWorkingDirectory(pid_t pid);
std::vector<std::string> ReadProcessCommandLine(pid_t pid);
ProcessIdentity ReadProcessIdentity(pid_t pid);
bool ProcessIdentityMatches(
    pid_t pid,
    uid_t expected_uid,
    const std::string& expected_start_time_ticks);
std::string UidToString(uid_t uid);
std::string GidToString(gid_t gid);
std::string PidToString(pid_t pid);
void CopyTree(const std::filesystem::path& source, const std::filesystem::path& destination);
void ChownTree(const std::filesystem::path& root, uid_t uid, gid_t gid);

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_UTIL_H_
