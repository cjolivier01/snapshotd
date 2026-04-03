#ifndef SNAPSHOT_CSRC_STORE_H_
#define SNAPSHOT_CSRC_STORE_H_

#include <sys/types.h>

#include <filesystem>
#include <string>

namespace snapshotd {

struct JobRecord {
  std::string job_id;
  uid_t owner_uid = 0;
  gid_t owner_gid = 0;
  pid_t pid = 0;
  std::string start_time_ticks;
  std::string executable;
  std::string cwd;
  std::string command_line;
  std::string state;
  std::string created_at;
  std::string latest_checkpoint;
};

struct CheckpointRecord {
  std::string checkpoint_id;
  std::string job_id;
  std::string state;
  std::string created_at;
  std::string dump_log;
  std::string restore_log;
  std::string restored_pid;
};

class Store {
 public:
  explicit Store(std::filesystem::path state_root);

  const std::filesystem::path& state_root() const { return state_root_; }
  void Initialize() const;

  std::filesystem::path UserDir(uid_t uid) const;
  std::filesystem::path JobsDir(uid_t uid) const;
  std::filesystem::path ExportsDir(uid_t uid) const;
  std::filesystem::path JobDir(uid_t uid, const std::string& job_id) const;
  std::filesystem::path JobMetaPath(uid_t uid, const std::string& job_id) const;
  std::filesystem::path CheckpointsDir(uid_t uid, const std::string& job_id) const;
  std::filesystem::path CheckpointDir(
      uid_t uid,
      const std::string& job_id,
      const std::string& checkpoint_id) const;
  std::filesystem::path CheckpointMetaPath(
      uid_t uid,
      const std::string& job_id,
      const std::string& checkpoint_id) const;
  std::filesystem::path ExportCheckpointDir(
      uid_t uid,
      const std::string& job_id,
      const std::string& checkpoint_id) const;

  JobRecord CreateJob(
      uid_t owner_uid,
      gid_t owner_gid,
      pid_t pid,
      const std::string& start_time_ticks,
      const std::string& executable,
      const std::string& cwd,
      const std::string& command_line) const;
  void SaveJob(const JobRecord& job) const;
  JobRecord LoadJob(uid_t owner_uid, const std::string& job_id) const;

  CheckpointRecord CreateCheckpoint(const JobRecord& job) const;
  void SaveCheckpoint(const JobRecord& job, const CheckpointRecord& checkpoint) const;
  CheckpointRecord LoadCheckpoint(
      const JobRecord& job,
      const std::string& checkpoint_id) const;
  std::string ResolveCheckpointId(
      const JobRecord& job,
      const std::string& requested_checkpoint_id) const;

 private:
  std::filesystem::path state_root_;
};

void AuthorizeJobAccess(const JobRecord& job, uid_t peer_uid);

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_STORE_H_
