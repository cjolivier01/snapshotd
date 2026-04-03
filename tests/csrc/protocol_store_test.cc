#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <functional>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>

#include "src/csrc/daemon.h"
#include "src/csrc/protocol.h"
#include "src/csrc/store.h"
#include "src/csrc/util.h"

namespace fs = std::filesystem;

namespace {

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void ExpectThrows(const std::function<void()>& fn, const std::string& label) {
  bool threw = false;
  try {
    fn();
  } catch (...) {
    threw = true;
  }
  Expect(threw, "expected exception for " + label);
}

void ExpectNoGroupOrOtherPermissions(const fs::path& path, const std::string& label) {
  const fs::perms perms = fs::status(path).permissions();
  const fs::perms disallowed = fs::perms::group_all | fs::perms::others_all;
  Expect((perms & disallowed) == fs::perms::none, label);
}

fs::path MakeTempDir(const std::string& prefix) {
  const fs::path path = fs::temp_directory_path() / snapshotd::GenerateId(prefix);
  snapshotd::EnsureDir(path, 0700);
  return path;
}

void TestProtocolRoundTrip() {
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    throw std::runtime_error("socketpair failed");
  }

  snapshotd::Message outgoing;
  outgoing.command = "checkpoint";
  outgoing.AddField("job_id", "job-123");
  outgoing.AddField("checkpoint_id", "ckpt-123");
  snapshotd::SendMessage(fds[0], outgoing);
  const snapshotd::Message incoming = snapshotd::ReceiveMessage(fds[1]);
  Expect(incoming.command == "checkpoint", "protocol command mismatch");
  Expect(incoming.Get("job_id") == "job-123", "protocol field mismatch");
  Expect(incoming.Get("checkpoint_id") == "ckpt-123", "protocol second field mismatch");

  const snapshotd::PeerCred cred = snapshotd::GetPeerCred(fds[0]);
  Expect(cred.uid == getuid(), "peer credential uid mismatch");

  close(fds[0]);
  close(fds[1]);
}

void TestValidateAllowedFieldsRejectsUnknownField() {
  snapshotd::Message request;
  request.command = "restore";
  request.AddField("job_id", "job-123");
  request.AddField("criu_arg", "--action-script=/tmp/evil");
  ExpectThrows(
      [&]() {
        snapshotd::ValidateAllowedFields(request, {"job_id", "checkpoint_id"});
      },
      "unknown request field");
}

void TestReceiveMessageRejectsOversizedPayload() {
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    throw std::runtime_error("socketpair failed");
  }
  const std::uint32_t size =
      static_cast<std::uint32_t>(snapshotd::kMaxControlMessageBytes + 1);
  if (write(fds[0], &size, sizeof(size)) != static_cast<ssize_t>(sizeof(size))) {
    throw std::runtime_error("failed to write oversized header");
  }
  ExpectThrows(
      [&]() {
        (void)snapshotd::ReceiveMessage(fds[1]);
      },
      "oversized control message");
  close(fds[0]);
  close(fds[1]);
}

void TestSendMessageClosedPeerThrowsInsteadOfSignaling() {
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    throw std::runtime_error("socketpair failed");
  }
  close(fds[1]);

  const pid_t child = fork();
  if (child < 0) {
    throw std::runtime_error("fork failed");
  }
  if (child == 0) {
    try {
      snapshotd::Message message;
      message.command = "status";
      message.AddField("job_id", "job-123");
      snapshotd::SendMessage(fds[0], message);
      _exit(1);
    } catch (...) {
      _exit(0);
    }
  }

  close(fds[0]);
  int status = 0;
  if (waitpid(child, &status, 0) < 0) {
    throw std::runtime_error("waitpid failed");
  }
  Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "SendMessage should throw on a closed peer instead of dying from SIGPIPE");
}

void TestStoreSaveLoadAndAuthorization() {
  const fs::path temp_dir = MakeTempDir("storetest");
  snapshotd::Store store(temp_dir);
  store.Initialize();
  Expect((fs::status(temp_dir).permissions() & fs::perms::owner_read) != fs::perms::none,
         "state root should remain traversable");

  snapshotd::JobRecord job =
      store.CreateJob(1001, 1001, 3456, "123456", "/bin/sleep", "/tmp", "/bin/sleep 30");
  store.SaveJob(job);
  ExpectNoGroupOrOtherPermissions(
      store.JobDir(job.owner_uid, job.job_id),
      "job directory should remain private");
  ExpectNoGroupOrOtherPermissions(
      store.JobMetaPath(job.owner_uid, job.job_id),
      "job metadata should remain private");
  snapshotd::JobRecord loaded = store.LoadJob(1001, job.job_id);
  Expect(loaded.job_id == job.job_id, "job id roundtrip mismatch");
  Expect(loaded.pid == 3456, "job pid roundtrip mismatch");
  Expect(loaded.start_time_ticks == "123456", "job start time roundtrip mismatch");
  Expect(loaded.state == "running", "job state roundtrip mismatch");

  snapshotd::CheckpointRecord checkpoint = store.CreateCheckpoint(loaded);
  checkpoint.state = "ready";
  checkpoint.restore_count = "2";
  checkpoint.last_restored_at = "200";
  checkpoint.size_bytes = "123";
  store.SaveCheckpoint(loaded, checkpoint);
  ExpectNoGroupOrOtherPermissions(
      store.CheckpointDir(loaded.owner_uid, loaded.job_id, checkpoint.checkpoint_id),
      "checkpoint directory should remain private");
  ExpectNoGroupOrOtherPermissions(
      store.CheckpointMetaPath(loaded.owner_uid, loaded.job_id, checkpoint.checkpoint_id),
      "checkpoint metadata should remain private");
  loaded.latest_checkpoint = checkpoint.checkpoint_id;
  store.SaveJob(loaded);

  const snapshotd::CheckpointRecord reloaded =
      store.LoadCheckpoint(loaded, checkpoint.checkpoint_id);
  Expect(reloaded.checkpoint_id == checkpoint.checkpoint_id, "checkpoint id mismatch");
  Expect(reloaded.restore_count == "2", "checkpoint restore_count mismatch");
  Expect(reloaded.last_restored_at == "200", "checkpoint last_restored_at mismatch");
  Expect(reloaded.size_bytes == "123", "checkpoint size_bytes mismatch");
  Expect(store.ResolveCheckpointId(loaded, "") == checkpoint.checkpoint_id,
         "latest checkpoint resolution mismatch");
  const fs::path export_dir =
      store.ExportCheckpointDir(loaded.owner_uid, loaded.job_id, checkpoint.checkpoint_id);
  Expect(
      export_dir.string().find("/exports/") != std::string::npos,
      "export checkpoint path should live under exports");

  snapshotd::AuthorizeJobAccess(loaded, 1001);
  ExpectThrows(
      [&]() { snapshotd::AuthorizeJobAccess(loaded, 2002); },
      "authorization mismatch");

  snapshotd::RemoveTree(temp_dir);
}

void TestPruneCheckpointsRemovesExpiredEntries() {
  const fs::path temp_dir = MakeTempDir("pruneage");
  snapshotd::Store store(temp_dir);
  store.Initialize();

  snapshotd::JobRecord job =
      store.CreateJob(1001, 1001, 2222, "10", "/bin/sleep", "/tmp", "/bin/sleep 30");
  store.SaveJob(job);

  snapshotd::CheckpointRecord expired = store.CreateCheckpoint(job);
  snapshotd::WriteTextFile(
      store.CheckpointDir(job.owner_uid, job.job_id, expired.checkpoint_id) / "images" / "old.bin",
      std::string(64, 'o'));
  expired.state = "ready";
  expired.created_at = "1";
  store.SaveCheckpoint(job, expired);

  snapshotd::CheckpointRecord latest = store.CreateCheckpoint(job);
  snapshotd::WriteTextFile(
      store.CheckpointDir(job.owner_uid, job.job_id, latest.checkpoint_id) / "images" / "new.bin",
      std::string(64, 'n'));
  latest.state = "ready";
  latest.created_at = std::to_string(static_cast<long long>(std::time(nullptr)));
  store.SaveCheckpoint(job, latest);

  job.latest_checkpoint = latest.checkpoint_id;
  store.SaveJob(job);

  snapshotd::DaemonConfig config;
  config.max_checkpoint_age_seconds = 30;
  config.min_keep_checkpoints_per_job = 1;
  config.max_keep_checkpoints_per_job = 5;

  const int removed = snapshotd::PruneCheckpoints(config, &store);
  Expect(removed == 1, "expected one expired checkpoint to be pruned");
  Expect(!snapshotd::PathExists(
             store.CheckpointDir(job.owner_uid, job.job_id, expired.checkpoint_id)),
         "expired checkpoint directory should be removed");
  Expect(snapshotd::PathExists(
             store.CheckpointDir(job.owner_uid, job.job_id, latest.checkpoint_id)),
         "latest checkpoint should be retained");

  snapshotd::RemoveTree(temp_dir);
}

void TestPruneCheckpointsPrefersColdEntriesUnderByteBudget() {
  const fs::path temp_dir = MakeTempDir("prunebudget");
  snapshotd::Store store(temp_dir);
  store.Initialize();

  snapshotd::JobRecord job =
      store.CreateJob(1001, 1001, 3333, "20", "/bin/sleep", "/tmp", "/bin/sleep 30");
  store.SaveJob(job);

  auto make_checkpoint = [&](const std::string& created_at, char fill) {
    snapshotd::CheckpointRecord checkpoint = store.CreateCheckpoint(job);
    snapshotd::WriteTextFile(
        store.CheckpointDir(job.owner_uid, job.job_id, checkpoint.checkpoint_id) / "images" /
            "payload.bin",
        std::string(128, fill));
    checkpoint.state = "ready";
    checkpoint.created_at = created_at;
    store.SaveCheckpoint(job, checkpoint);
    checkpoint = store.LoadCheckpoint(job, checkpoint.checkpoint_id);
    checkpoint.size_bytes = std::to_string(snapshotd::DirectoryTreeSizeBytes(
        store.CheckpointDir(job.owner_uid, job.job_id, checkpoint.checkpoint_id)));
    store.SaveCheckpoint(job, checkpoint);
    return checkpoint;
  };

  snapshotd::CheckpointRecord cold = make_checkpoint("100", 'c');
  snapshotd::CheckpointRecord hot = make_checkpoint("200", 'h');
  hot.restore_count = "4";
  hot.last_restored_at = "500";
  store.SaveCheckpoint(job, hot);
  snapshotd::CheckpointRecord latest = make_checkpoint("300", 'l');

  job.latest_checkpoint = latest.checkpoint_id;
  store.SaveJob(job);

  const std::uint64_t latest_size = snapshotd::DirectoryTreeSizeBytes(
      store.CheckpointDir(job.owner_uid, job.job_id, latest.checkpoint_id));
  const std::uint64_t hot_size = snapshotd::DirectoryTreeSizeBytes(
      store.CheckpointDir(job.owner_uid, job.job_id, hot.checkpoint_id));

  snapshotd::DaemonConfig config;
  config.max_checkpoint_age_seconds = 0;
  config.min_keep_checkpoints_per_job = 1;
  config.max_keep_checkpoints_per_job = 5;
  config.max_bytes_total = latest_size + hot_size + 1;

  const int removed = snapshotd::PruneCheckpoints(config, &store);
  Expect(removed == 1, "expected one cold checkpoint to be pruned under byte budget");
  Expect(!snapshotd::PathExists(
             store.CheckpointDir(job.owner_uid, job.job_id, cold.checkpoint_id)),
         "cold checkpoint should be removed");
  Expect(snapshotd::PathExists(
             store.CheckpointDir(job.owner_uid, job.job_id, hot.checkpoint_id)),
         "hot checkpoint should be retained");
  Expect(snapshotd::PathExists(
             store.CheckpointDir(job.owner_uid, job.job_id, latest.checkpoint_id)),
         "latest checkpoint should be retained");

  snapshotd::RemoveTree(temp_dir);
}

void TestWriteTextFilePreservesPrivateParentPermissions() {
  const fs::path temp_dir = MakeTempDir("writetext");
  const fs::path private_dir = temp_dir / "private";
  snapshotd::EnsureDir(private_dir, 0700);
  snapshotd::WriteTextFile(private_dir / "secret.txt", "secret\n", 0600, 0700);

  ExpectNoGroupOrOtherPermissions(
      private_dir,
      "WriteTextFile should not relax private parent directory permissions");
  ExpectNoGroupOrOtherPermissions(
      private_dir / "secret.txt",
      "WriteTextFile should create private files when requested");

  snapshotd::RemoveTree(temp_dir);
}

void TestSafeIdValidation() {
  Expect(snapshotd::IsSafeId("job-abc123"), "expected safe id");
  Expect(!snapshotd::IsSafeId("../etc/passwd"), "expected path traversal to be unsafe");
  Expect(!snapshotd::IsSafeId("bad/value"), "expected slash to be unsafe");
  ExpectThrows(
      []() { snapshotd::RequireSafeId("../evil", "job_id"); },
      "unsafe id rejection");
}

}  // namespace

int main() {
  try {
    TestProtocolRoundTrip();
    TestValidateAllowedFieldsRejectsUnknownField();
    TestReceiveMessageRejectsOversizedPayload();
    TestSendMessageClosedPeerThrowsInsteadOfSignaling();
    TestStoreSaveLoadAndAuthorization();
    TestPruneCheckpointsRemovesExpiredEntries();
    TestPruneCheckpointsPrefersColdEntriesUnderByteBudget();
    TestWriteTextFilePreservesPrivateParentPermissions();
    TestSafeIdValidation();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
