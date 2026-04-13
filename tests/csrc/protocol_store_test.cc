/** @file
 *  @brief Unit coverage for control-message framing, store persistence, and retention.
 *
 *  @details
 *  These tests concentrate on the broker's non-CRIU foundations: control
 *  protocol safety, metadata persistence, permission boundaries, and retention
 *  pruning decisions.
 */

#include <fcntl.h>
#include <pty.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <cstring>
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

/** @brief Minimal assertion helper that throws with a readable failure message. */
void Expect(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

/** @brief Assert that a callable throws, for negative-path validation tests. */
void ExpectThrows(const std::function<void()>& fn, const std::string& label) {
  bool threw = false;
  try {
    fn();
  } catch (...) {
    threw = true;
  }
  Expect(threw, "expected exception for " + label);
}

/** @brief Verify that broker-owned paths remain private to the owning user. */
void ExpectNoGroupOrOtherPermissions(const fs::path& path, const std::string& label) {
  const fs::perms perms = fs::status(path).permissions();
  const fs::perms disallowed = fs::perms::group_all | fs::perms::others_all;
  Expect((perms & disallowed) == fs::perms::none, label);
}

/** @brief Create one unique private temporary directory for a test case. */
fs::path MakeTempDir(const std::string& prefix) {
  const fs::path path = fs::temp_directory_path() / snapshotd::GenerateId(prefix);
  snapshotd::EnsureDir(path, 0700);
  return path;
}

/** @brief Verify basic message framing and peer-credential lookup. */
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

/** @brief Ensure request allowlisting rejects unknown control fields. */
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

/** @brief Ensure oversized frames are rejected before payload allocation. */
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

/** @brief Ensure closed peers raise exceptions instead of killing the sender with SIGPIPE. */
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

/** @brief Verify job/checkpoint persistence, path layout, and authorization checks. */
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

/** @brief Verify age-based retention pruning deletes expired checkpoints first. */
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

/** @brief Verify byte-budget pruning prefers colder checkpoints over hot/latest ones. */
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
    snapshotd::WriteTextFile(
        store.ExportCheckpointDir(job.owner_uid, job.job_id, checkpoint.checkpoint_id) /
            "images" / "payload.bin",
        std::string(128, static_cast<char>(fill + 1)),
        0644,
        0755);
    checkpoint.state = "ready";
    checkpoint.created_at = created_at;
    store.SaveCheckpoint(job, checkpoint);
    checkpoint = store.LoadCheckpoint(job, checkpoint.checkpoint_id);
    checkpoint.size_bytes = std::to_string(
        snapshotd::DirectoryTreeSizeBytes(
            store.CheckpointDir(job.owner_uid, job.job_id, checkpoint.checkpoint_id)) +
        snapshotd::DirectoryTreeSizeBytes(
            store.ExportCheckpointDir(job.owner_uid, job.job_id, checkpoint.checkpoint_id)));
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

  const std::uint64_t latest_size =
      snapshotd::DirectoryTreeSizeBytes(
          store.CheckpointDir(job.owner_uid, job.job_id, latest.checkpoint_id)) +
      snapshotd::DirectoryTreeSizeBytes(
          store.ExportCheckpointDir(job.owner_uid, job.job_id, latest.checkpoint_id));
  const std::uint64_t hot_size =
      snapshotd::DirectoryTreeSizeBytes(
          store.CheckpointDir(job.owner_uid, job.job_id, hot.checkpoint_id)) +
      snapshotd::DirectoryTreeSizeBytes(
          store.ExportCheckpointDir(job.owner_uid, job.job_id, hot.checkpoint_id));

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

/** @brief Ensure private parent directory modes stay private after file writes. */
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

/** @brief Verify the broker's identifier sanitizer rejects traversal-oriented values. */
void TestSafeIdValidation() {
  Expect(snapshotd::IsSafeId("job-abc123"), "expected safe id");
  Expect(!snapshotd::IsSafeId("../etc/passwd"), "expected path traversal to be unsafe");
  Expect(!snapshotd::IsSafeId("bad/value"), "expected slash to be unsafe");
  ExpectThrows(
      []() { snapshotd::RequireSafeId("../evil", "job_id"); },
      "unsafe id rejection");
}

// --- SCM_RIGHTS fd-passing tests ---

/** @brief Verify a framed message and attached fd survive a socket round-trip. */
void TestSendReceiveMessageWithFdRoundTrip() {
  // Verify a message + ancillary fd survive a socketpair roundtrip.
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    throw std::runtime_error("socketpair failed");
  }

  // Create a pipe so we have a recognizable fd to send.
  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    throw std::runtime_error("pipe failed");
  }

  snapshotd::Message outgoing;
  outgoing.command = "restore";
  outgoing.AddField("job_id", "job-fd-test");
  snapshotd::SendMessageWithFd(fds[0], outgoing, pipe_fds[0]);

  int received_fd = -1;
  const snapshotd::Message incoming = snapshotd::ReceiveMessageWithFd(fds[1], &received_fd);
  Expect(incoming.command == "restore", "fd roundtrip command mismatch");
  Expect(incoming.Get("job_id") == "job-fd-test", "fd roundtrip field mismatch");
  Expect(received_fd >= 0, "expected to receive a file descriptor");

  // Verify the received fd is a valid dup of the pipe read end: write to the
  // pipe write end and read from the received fd.
  const char test_data[] = "hello";
  Expect(write(pipe_fds[1], test_data, sizeof(test_data)) == static_cast<ssize_t>(sizeof(test_data)),
         "pipe write failed");
  char buf[sizeof(test_data)] = {};
  Expect(read(received_fd, buf, sizeof(buf)) == static_cast<ssize_t>(sizeof(test_data)),
         "pipe read from received fd failed");
  Expect(std::string(buf, sizeof(test_data)) == std::string(test_data, sizeof(test_data)),
         "data read from received fd does not match");

  close(received_fd);
  close(pipe_fds[0]);
  close(pipe_fds[1]);
  close(fds[0]);
  close(fds[1]);
}

/** @brief Verify the fd-passing path behaves normally when no fd is attached. */
void TestSendReceiveMessageWithNoFd() {
  // Sending with ancillary_fd = -1 should behave like a normal message.
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    throw std::runtime_error("socketpair failed");
  }

  snapshotd::Message outgoing;
  outgoing.command = "status";
  outgoing.AddField("key", "value");
  snapshotd::SendMessageWithFd(fds[0], outgoing, -1);

  int received_fd = -1;
  const snapshotd::Message incoming = snapshotd::ReceiveMessageWithFd(fds[1], &received_fd);
  Expect(incoming.command == "status", "no-fd command mismatch");
  Expect(incoming.Get("key") == "value", "no-fd field mismatch");
  Expect(received_fd == -1, "should not receive an fd when none was sent");

  close(fds[0]);
  close(fds[1]);
}

/** @brief Verify PTY descriptors remain TTYs after SCM_RIGHTS duplication. */
void TestSendMessageWithFdToPty() {
  // Verify that a PTY fd can be sent and the receiver sees it as a tty.
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    throw std::runtime_error("socketpair failed");
  }

  int master_fd = -1;
  int slave_fd = -1;
  if (openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr) != 0) {
    throw std::runtime_error("openpty failed");
  }

  snapshotd::Message outgoing;
  outgoing.command = "restore";
  snapshotd::SendMessageWithFd(fds[0], outgoing, slave_fd);

  int received_fd = -1;
  snapshotd::ReceiveMessageWithFd(fds[1], &received_fd);
  Expect(received_fd >= 0, "expected to receive pty fd");
  Expect(isatty(received_fd) == 1, "received fd should be a tty");

  // Verify it's a distinct fd number from the original.
  Expect(received_fd != slave_fd, "received fd should be a kernel-duplicated copy");

  // Verify the sender still owns the original fd (not closed).
  struct stat stat_buf {};
  Expect(fstat(slave_fd, &stat_buf) == 0, "original slave_fd should still be valid after send");

  close(received_fd);
  close(slave_fd);
  close(master_fd);
  close(fds[0]);
  close(fds[1]);
}

/** @brief Verify oversized fd-bearing frames are rejected and cleaned up safely. */
void TestReceiveMessageWithFdRejectsOversized() {
  // Oversized messages with an attached fd should close the fd and throw.
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    throw std::runtime_error("socketpair failed");
  }

  // Manually write an oversized header with an ancillary fd.
  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    throw std::runtime_error("pipe failed");
  }

  const std::uint32_t bad_size =
      static_cast<std::uint32_t>(snapshotd::kMaxControlMessageBytes + 1);

  // Build the sendmsg with the oversized header and attached fd.
  struct iovec iov {};
  iov.iov_base = const_cast<std::uint32_t*>(&bad_size);
  iov.iov_len = sizeof(bad_size);
  union {
    struct cmsghdr align;
    char buf[CMSG_SPACE(sizeof(int))];
  } cmsg_buf;
  memset(&cmsg_buf, 0, sizeof(cmsg_buf));
  struct msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf.buf;
  msg.msg_controllen = sizeof(cmsg_buf.buf);
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &pipe_fds[0], sizeof(int));
  Expect(sendmsg(fds[0], &msg, MSG_NOSIGNAL) > 0, "sendmsg should succeed");

  int received_fd = -1;
  ExpectThrows(
      [&]() {
        (void)snapshotd::ReceiveMessageWithFd(fds[1], &received_fd);
      },
      "oversized message with fd");
  // The received fd should be cleaned up (set to -1) by ReceiveMessageWithFd.
  Expect(received_fd == -1, "fd should be closed on oversized reject");

  close(pipe_fds[0]);
  close(pipe_fds[1]);
  close(fds[0]);
  close(fds[1]);
}

/** @brief Verify fd passing still works when the message spans multiple sendmsg calls. */
void TestFdPassingWithLargeMessage() {
  // Verify fd passing works with a message that requires multiple sendmsg/recvmsg calls.
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    throw std::runtime_error("socketpair failed");
  }

  snapshotd::Message outgoing;
  outgoing.command = "restore";
  // Add many fields to make the message larger.
  for (int index = 0; index < 500; ++index) {
    outgoing.AddField("field_" + std::to_string(index), std::string(100, 'x'));
  }

  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    throw std::runtime_error("pipe failed");
  }

  snapshotd::SendMessageWithFd(fds[0], outgoing, pipe_fds[0]);

  int received_fd = -1;
  const snapshotd::Message incoming = snapshotd::ReceiveMessageWithFd(fds[1], &received_fd);
  Expect(incoming.command == "restore", "large message command mismatch");
  Expect(incoming.Get("field_0") == std::string(100, 'x'), "large message field mismatch");
  Expect(incoming.Get("field_499") == std::string(100, 'x'), "large message last field mismatch");
  Expect(received_fd >= 0, "expected fd with large message");

  // Verify the fd is usable.
  const char probe[] = "ok";
  Expect(write(pipe_fds[1], probe, 2) == 2, "pipe write failed");
  char result[2] = {};
  Expect(read(received_fd, result, 2) == 2, "pipe read from received fd failed");
  Expect(result[0] == 'o' && result[1] == 'k', "data mismatch on large message fd");

  close(received_fd);
  close(pipe_fds[0]);
  close(pipe_fds[1]);
  close(fds[0]);
  close(fds[1]);
}

void TestSendWithFdReceiveWithPlainReceiveMessage() {
  // Sending with an fd but receiving with the plain ReceiveMessage should
  // still decode the message correctly (the fd is silently discarded by the
  // kernel since recvmsg without an ancillary buffer drops them).
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    throw std::runtime_error("socketpair failed");
  }

  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    throw std::runtime_error("pipe failed");
  }

  snapshotd::Message outgoing;
  outgoing.command = "checkpoint";
  outgoing.AddField("job_id", "compat-test");
  snapshotd::SendMessageWithFd(fds[0], outgoing, pipe_fds[0]);

  // Use plain ReceiveMessage (no fd support) — should still work.
  const snapshotd::Message incoming = snapshotd::ReceiveMessage(fds[1]);
  Expect(incoming.command == "checkpoint", "compat command mismatch");
  Expect(incoming.Get("job_id") == "compat-test", "compat field mismatch");

  close(pipe_fds[0]);
  close(pipe_fds[1]);
  close(fds[0]);
  close(fds[1]);
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
    TestSendReceiveMessageWithFdRoundTrip();
    TestSendReceiveMessageWithNoFd();
    TestSendMessageWithFdToPty();
    TestReceiveMessageWithFdRejectsOversized();
    TestFdPassingWithLargeMessage();
    TestSendWithFdReceiveWithPlainReceiveMessage();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
