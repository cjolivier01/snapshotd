/** @file
 *  @brief Filesystem-backed metadata store for jobs, checkpoints, and exports.
 *
 *  @details
 *  The store separates authoritative private restore inputs from user-readable
 *  compatibility exports. That separation is relied upon by the daemon's
 *  restore and retention policy.
 */

#include "src/csrc/store.h"

#include <unistd.h>

#include <ctime>
#include <vector>
#include <map>
#include <stdexcept>

#include "src/csrc/util.h"

namespace snapshotd {

namespace fs = std::filesystem;

namespace {

/** @brief Return the current wall-clock timestamp serialized in epoch seconds. */
std::string CurrentTimestamp() {
  const std::time_t now = std::time(nullptr);
  return std::to_string(static_cast<long long>(now));
}

/** @brief Rehydrate one persisted job metadata map into a typed record. */
JobRecord ParseJobRecord(const std::map<std::string, std::string>& data) {
  JobRecord record;
  record.job_id = data.at("job_id");
  record.owner_uid = static_cast<uid_t>(std::stoul(data.at("owner_uid")));
  record.owner_gid = static_cast<gid_t>(std::stoul(data.at("owner_gid")));
  record.pid = static_cast<pid_t>(std::stol(data.at("pid")));
  const auto start_time = data.find("start_time_ticks");
  if (start_time != data.end()) {
    record.start_time_ticks = start_time->second;
  }
  record.executable = data.at("executable");
  record.cwd = data.at("cwd");
  record.command_line = data.at("command_line");
  record.state = data.at("state");
  record.created_at = data.at("created_at");
  const auto latest = data.find("latest_checkpoint");
  if (latest != data.end()) {
    record.latest_checkpoint = latest->second;
  }
  return record;
}

/** @brief Rehydrate one persisted checkpoint metadata map into a typed record. */
CheckpointRecord ParseCheckpointRecord(const std::map<std::string, std::string>& data) {
  CheckpointRecord record;
  record.checkpoint_id = data.at("checkpoint_id");
  record.job_id = data.at("job_id");
  record.state = data.at("state");
  record.created_at = data.at("created_at");
  const auto last_restored = data.find("last_restored_at");
  if (last_restored != data.end()) {
    record.last_restored_at = last_restored->second;
  }
  const auto restore_count = data.find("restore_count");
  if (restore_count != data.end()) {
    record.restore_count = restore_count->second;
  }
  const auto size_bytes = data.find("size_bytes");
  if (size_bytes != data.end()) {
    record.size_bytes = size_bytes->second;
  }
  record.dump_log = data.at("dump_log");
  record.restore_log = data.at("restore_log");
  const auto restored = data.find("restored_pid");
  if (restored != data.end()) {
    record.restored_pid = restored->second;
  }
  return record;
}

}  // namespace

/** @brief Point a Store instance at the root of broker-owned state. */
Store::Store(fs::path state_root) : state_root_(std::move(state_root)) {}

void Store::Initialize() const {
  EnsureDir(state_root_, 0755);
}

fs::path Store::UserDir(uid_t uid) const {
  return state_root_ / UidToString(uid);
}

fs::path Store::JobsDir(uid_t uid) const {
  return UserDir(uid) / "jobs";
}

fs::path Store::ExportsDir(uid_t uid) const {
  return UserDir(uid) / "exports";
}

fs::path Store::JobDir(uid_t uid, const std::string& job_id) const {
  RequireSafeId(job_id, "job_id");
  return JobsDir(uid) / job_id;
}

fs::path Store::JobMetaPath(uid_t uid, const std::string& job_id) const {
  return JobDir(uid, job_id) / "job.meta";
}

fs::path Store::CheckpointsDir(uid_t uid, const std::string& job_id) const {
  return JobDir(uid, job_id) / "checkpoints";
}

fs::path Store::CheckpointDir(
    uid_t uid,
    const std::string& job_id,
    const std::string& checkpoint_id) const {
  RequireSafeId(checkpoint_id, "checkpoint_id");
  return CheckpointsDir(uid, job_id) / checkpoint_id;
}

fs::path Store::CheckpointMetaPath(
    uid_t uid,
    const std::string& job_id,
    const std::string& checkpoint_id) const {
  return CheckpointDir(uid, job_id, checkpoint_id) / "checkpoint.meta";
}

fs::path Store::ExportCheckpointDir(
    uid_t uid,
    const std::string& job_id,
    const std::string& checkpoint_id) const {
  RequireSafeId(job_id, "job_id");
  RequireSafeId(checkpoint_id, "checkpoint_id");
  return ExportsDir(uid) / job_id / checkpoint_id;
}

JobRecord Store::CreateJob(
    uid_t owner_uid,
    gid_t owner_gid,
    pid_t pid,
    const std::string& start_time_ticks,
    const std::string& executable,
    const std::string& cwd,
    const std::string& command_line) const {
  JobRecord job;
  job.job_id = GenerateId("job");
  job.owner_uid = owner_uid;
  job.owner_gid = owner_gid;
  job.pid = pid;
  job.start_time_ticks = start_time_ticks;
  job.executable = executable;
  job.cwd = cwd;
  job.command_line = command_line;
  job.state = "running";
  job.created_at = CurrentTimestamp();
  return job;
}

void Store::SaveJob(const JobRecord& job) const {
  RequireSafeId(job.job_id, "job_id");
  EnsureDir(UserDir(job.owner_uid), 0755);
  EnsureDir(JobsDir(job.owner_uid), 0700);
  EnsureDir(JobDir(job.owner_uid, job.job_id), 0700);
  std::map<std::string, std::string> data = {
      {"job_id", job.job_id},
      {"owner_uid", UidToString(job.owner_uid)},
      {"owner_gid", GidToString(job.owner_gid)},
      {"pid", PidToString(job.pid)},
      {"start_time_ticks", job.start_time_ticks},
      {"executable", job.executable},
      {"cwd", job.cwd},
      {"command_line", job.command_line},
      {"state", job.state},
      {"created_at", job.created_at},
      {"latest_checkpoint", job.latest_checkpoint},
  };
  WriteTextFile(
      JobMetaPath(job.owner_uid, job.job_id),
      SerializeKeyValueMap(data),
      0600,
      0700);
}

JobRecord Store::LoadJob(uid_t owner_uid, const std::string& job_id) const {
  const fs::path path = JobMetaPath(owner_uid, job_id);
  if (!PathExists(path)) {
    throw std::runtime_error("job not found: " + job_id);
  }
  return ParseJobRecord(ParseKeyValueText(ReadTextFile(path)));
}

std::vector<uid_t> Store::ListUsers() const {
  std::vector<uid_t> users;
  if (!PathExists(state_root_)) {
    return users;
  }
  for (const auto& entry : fs::directory_iterator(state_root_)) {
    if (!entry.is_directory()) {
      continue;
    }
    try {
      users.push_back(static_cast<uid_t>(std::stoul(entry.path().filename().string())));
    } catch (const std::exception&) {
      continue;
    }
  }
  return users;
}

std::vector<JobRecord> Store::ListJobs(uid_t owner_uid) const {
  std::vector<JobRecord> jobs;
  const fs::path jobs_dir = JobsDir(owner_uid);
  if (!PathExists(jobs_dir)) {
    return jobs;
  }
  for (const auto& entry : fs::directory_iterator(jobs_dir)) {
    if (!entry.is_directory()) {
      continue;
    }
    const std::string job_id = entry.path().filename().string();
    const fs::path metadata_path = JobMetaPath(owner_uid, job_id);
    if (!PathExists(metadata_path)) {
      continue;
    }
    jobs.push_back(ParseJobRecord(ParseKeyValueText(ReadTextFile(metadata_path))));
  }
  return jobs;
}

CheckpointRecord Store::CreateCheckpoint(const JobRecord& job) const {
  CheckpointRecord record;
  record.checkpoint_id = GenerateId("ckpt");
  record.job_id = job.job_id;
  record.state = "created";
  record.created_at = CurrentTimestamp();
  record.last_restored_at.clear();
  record.restore_count = "0";
  record.size_bytes = "0";
  EnsureDir(UserDir(job.owner_uid), 0755);
  EnsureDir(JobsDir(job.owner_uid), 0700);
  EnsureDir(JobDir(job.owner_uid, job.job_id), 0700);
  EnsureDir(CheckpointsDir(job.owner_uid, job.job_id), 0700);
  const fs::path checkpoint_dir =
      CheckpointDir(job.owner_uid, job.job_id, record.checkpoint_id);
  // Restore authority stays in the private tree. User-readable exports are
  // materialized separately by the daemon after the worker finishes.
  EnsureDir(checkpoint_dir / "images", 0700);
  EnsureDir(checkpoint_dir / "work", 0700);
  EnsureDir(checkpoint_dir / "logs", 0700);
  record.dump_log = (checkpoint_dir / "logs" / "dump.log").string();
  record.restore_log = (checkpoint_dir / "logs" / "restore.log").string();
  return record;
}

void Store::SaveCheckpoint(const JobRecord& job, const CheckpointRecord& checkpoint) const {
  RequireSafeId(checkpoint.checkpoint_id, "checkpoint_id");
  const fs::path checkpoint_dir =
      CheckpointDir(job.owner_uid, job.job_id, checkpoint.checkpoint_id);
  EnsureDir(checkpoint_dir, 0700);
  std::map<std::string, std::string> data = {
      {"checkpoint_id", checkpoint.checkpoint_id},
      {"job_id", checkpoint.job_id},
      {"state", checkpoint.state},
      {"created_at", checkpoint.created_at},
      {"last_restored_at", checkpoint.last_restored_at},
      {"restore_count", checkpoint.restore_count},
      {"size_bytes", checkpoint.size_bytes},
      {"dump_log", checkpoint.dump_log},
      {"restore_log", checkpoint.restore_log},
      {"restored_pid", checkpoint.restored_pid},
  };
  WriteTextFile(
      CheckpointMetaPath(job.owner_uid, job.job_id, checkpoint.checkpoint_id),
      SerializeKeyValueMap(data),
      0600,
      0700);
}

CheckpointRecord Store::LoadCheckpoint(
    const JobRecord& job,
    const std::string& checkpoint_id) const {
  const fs::path path =
      CheckpointMetaPath(job.owner_uid, job.job_id, checkpoint_id);
  if (!PathExists(path)) {
    throw std::runtime_error("checkpoint not found: " + checkpoint_id);
  }
  return ParseCheckpointRecord(ParseKeyValueText(ReadTextFile(path)));
}

std::vector<CheckpointRecord> Store::ListCheckpoints(const JobRecord& job) const {
  std::vector<CheckpointRecord> checkpoints;
  const fs::path checkpoints_dir = CheckpointsDir(job.owner_uid, job.job_id);
  if (!PathExists(checkpoints_dir)) {
    return checkpoints;
  }
  for (const auto& entry : fs::directory_iterator(checkpoints_dir)) {
    if (!entry.is_directory()) {
      continue;
    }
    const std::string checkpoint_id = entry.path().filename().string();
    const fs::path metadata_path =
        CheckpointMetaPath(job.owner_uid, job.job_id, checkpoint_id);
    if (!PathExists(metadata_path)) {
      continue;
    }
    checkpoints.push_back(ParseCheckpointRecord(ParseKeyValueText(ReadTextFile(metadata_path))));
  }
  return checkpoints;
}

void Store::RemoveCheckpoint(const JobRecord& job, const CheckpointRecord& checkpoint) const {
  RemoveTree(CheckpointDir(job.owner_uid, job.job_id, checkpoint.checkpoint_id));
  const fs::path export_dir =
      ExportCheckpointDir(job.owner_uid, job.job_id, checkpoint.checkpoint_id);
  if (PathExists(export_dir)) {
    RemoveTree(export_dir);
  }
}

std::string Store::ResolveCheckpointId(
    const JobRecord& job,
    const std::string& requested_checkpoint_id) const {
  if (!requested_checkpoint_id.empty()) {
    RequireSafeId(requested_checkpoint_id, "checkpoint_id");
    return requested_checkpoint_id;
  }
  if (job.latest_checkpoint.empty()) {
    throw std::runtime_error("job has no checkpoint");
  }
  RequireSafeId(job.latest_checkpoint, "checkpoint_id");
  return job.latest_checkpoint;
}

void AuthorizeJobAccess(const JobRecord& job, uid_t peer_uid) {
  if (job.owner_uid != peer_uid) {
    throw std::runtime_error("job belongs to another uid");
  }
}

}  // namespace snapshotd
