/** @file
 *  @brief Persistent job and checkpoint metadata for snapshotd.
 */

#ifndef SNAPSHOT_CSRC_STORE_H_
#define SNAPSHOT_CSRC_STORE_H_

#include <sys/types.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace snapshotd {

/** @brief Broker-owned metadata for one managed process tree. */
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

/** @brief Broker-owned metadata for one checkpoint of a managed job. */
struct CheckpointRecord {
  std::string checkpoint_id;
  std::string job_id;
  std::string state;
  std::string created_at;
  std::string last_restored_at;
  std::string restore_count;
  std::string size_bytes;
  std::string dump_log;
  std::string restore_log;
  std::string restored_pid;
};

/**
 * @brief Root-owned filesystem store for managed jobs, checkpoints, and exports.
 *
 * Authoritative restore inputs live under the private `jobs/` subtree. User-readable
 * compatibility copies live under the separate `exports/` subtree.
 */
class Store {
 public:
  /** @brief Construct a store rooted at @p state_root. */
  explicit Store(std::filesystem::path state_root);

  /** @brief Return the configured root directory for all broker state. */
  const std::filesystem::path& state_root() const { return state_root_; }
  /** @brief Ensure the top-level state directory exists. */
  void Initialize() const;

  /** @brief Directory that holds all state for one caller UID. */
  std::filesystem::path UserDir(uid_t uid) const;
  /** @brief Private subtree that holds authoritative job metadata and images. */
  std::filesystem::path JobsDir(uid_t uid) const;
  /** @brief User-readable export subtree for compatibility artifacts. */
  std::filesystem::path ExportsDir(uid_t uid) const;
  /** @brief Private directory for one managed job. */
  std::filesystem::path JobDir(uid_t uid, const std::string& job_id) const;
  /** @brief Metadata file for one managed job. */
  std::filesystem::path JobMetaPath(uid_t uid, const std::string& job_id) const;
  /** @brief Directory that contains all checkpoints for a managed job. */
  std::filesystem::path CheckpointsDir(uid_t uid, const std::string& job_id) const;
  /** @brief Private directory for one checkpoint. */
  std::filesystem::path CheckpointDir(
      uid_t uid,
      const std::string& job_id,
      const std::string& checkpoint_id) const;
  /** @brief Metadata file for one checkpoint. */
  std::filesystem::path CheckpointMetaPath(
      uid_t uid,
      const std::string& job_id,
      const std::string& checkpoint_id) const;
  /** @brief Export directory for user-readable copies of checkpoint artifacts. */
  std::filesystem::path ExportCheckpointDir(
      uid_t uid,
      const std::string& job_id,
      const std::string& checkpoint_id) const;

  /** @brief Create a fresh in-memory job record. */
  JobRecord CreateJob(
      uid_t owner_uid,
      gid_t owner_gid,
      pid_t pid,
      const std::string& start_time_ticks,
      const std::string& executable,
      const std::string& cwd,
      const std::string& command_line) const;
  /** @brief Persist a job record to the private store. */
  void SaveJob(const JobRecord& job) const;
  /** @brief Load one job record owned by @p owner_uid. */
  JobRecord LoadJob(uid_t owner_uid, const std::string& job_id) const;
  /** @brief Enumerate known caller UIDs with broker-owned state. */
  std::vector<uid_t> ListUsers() const;
  /** @brief Enumerate managed jobs for one caller UID. */
  std::vector<JobRecord> ListJobs(uid_t owner_uid) const;

  /** @brief Create a fresh checkpoint record and private directory layout. */
  CheckpointRecord CreateCheckpoint(const JobRecord& job) const;
  /** @brief Persist checkpoint metadata to the private store. */
  void SaveCheckpoint(const JobRecord& job, const CheckpointRecord& checkpoint) const;
  /** @brief Load one checkpoint record for a managed job. */
  CheckpointRecord LoadCheckpoint(
      const JobRecord& job,
      const std::string& checkpoint_id) const;
  /** @brief Enumerate checkpoints for one managed job. */
  std::vector<CheckpointRecord> ListCheckpoints(const JobRecord& job) const;
  /** @brief Remove a checkpoint from both the private and export trees. */
  void RemoveCheckpoint(const JobRecord& job, const CheckpointRecord& checkpoint) const;
  /** @brief Pick an explicit checkpoint id, or fall back to the job's latest checkpoint. */
  std::string ResolveCheckpointId(
      const JobRecord& job,
      const std::string& requested_checkpoint_id) const;

 private:
  std::filesystem::path state_root_;
};

/** @brief Reject access to jobs that do not belong to the connected peer UID. */
void AuthorizeJobAccess(const JobRecord& job, uid_t peer_uid);

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_STORE_H_
