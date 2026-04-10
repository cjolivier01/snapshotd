/** @file
 *  @brief Public entry points for the privileged snapshotd broker.
 */

#ifndef SNAPSHOT_CSRC_DAEMON_H_
#define SNAPSHOT_CSRC_DAEMON_H_

#include <cstdint>
#include <filesystem>
#include <string>

#include "src/csrc/protocol.h"
#include "src/csrc/store.h"

namespace snapshotd {

/**
 * @defgroup daemon_api Privileged Broker
 * @brief Policy-enforcing daemon entry points and retention controls.
 *
 * This group is the core of the trust boundary. It authenticates peers,
 * validates request fields, launches managed jobs as the caller, invokes the
 * short-lived worker for CRIU operations, and applies daemon-owned retention.
 *
 * @see @ref it_review_guide
 * @see @ref safe_root_criu_broker_design
 * @{
 */

/** @brief Runtime configuration for the long-lived broker process. */
struct DaemonConfig {
  /** @brief Unix-domain control socket path, or the systemd-activated socket. */
  std::string socket_path;
  /** @brief Root-owned state directory that holds jobs and checkpoints. */
  std::filesystem::path state_dir;
  /** @brief Absolute path to the short-lived worker executable. */
  std::string worker_path;
  /** @brief Absolute path to the CRIU binary used for host-pid flows. */
  std::string criu_bin;
  /** @brief Absolute path to the helper binary used for pid-namespace dump flows. */
  std::string criu_ns_bin;
  /** @brief Upper bound for one worker invocation before the daemon cancels it. */
  int worker_timeout_seconds = 600;
  /** @brief Age limit for checkpoints; values <= 0 disable age-based pruning. */
  int max_checkpoint_age_seconds = 30 * 24 * 3600;
  /** @brief Minimum number of checkpoints to keep per job. */
  int min_keep_checkpoints_per_job = 1;
  /** @brief Maximum number of checkpoints to keep per job. */
  int max_keep_checkpoints_per_job = 5;
  /** @brief Per-user authoritative checkpoint byte budget; 0 disables it. */
  std::uint64_t max_bytes_per_user = 0;
  /** @brief Global authoritative checkpoint byte budget; 0 disables it. */
  std::uint64_t max_bytes_total = 0;
  /** @brief Optional file touched once the daemon is listening, used by tests. */
  std::string ready_file;
};

/**
 * @brief Handle a single validated request from a connected client.
 *
 * @param request Decoded request message.
 * @param peer Caller identity obtained from SO_PEERCRED.
 * @param config Daemon configuration.
 * @param store Persistent metadata store.
 * @param client_tty_fd Optional TTY fd received via SCM_RIGHTS (-1 if none).
 *        Ownership is NOT transferred; the caller must close it.
 * @return Reply message to send back on the same socket.
 *
 * This is the main policy gate for the broker. Every accepted operation must
 * stay within the narrow API described in the design documents.
 */
Message HandleRequest(
    const Message& request,
    const PeerCred& peer,
    const DaemonConfig& config,
    Store* store,
    int client_tty_fd = -1);
/** @brief Apply retention and space-budget pruning to persisted checkpoints. */
int PruneCheckpoints(
    const DaemonConfig& config,
    Store* store,
    const std::string& preserve_job_id = "",
    const std::string& preserve_checkpoint_id = "");

/** @brief Run the daemon event loop until shutdown. */
int RunDaemon(const DaemonConfig& config);
/** @brief Parse CLI flags and invoke @ref RunDaemon. */
int RunDaemonMain(int argc, char** argv);

/** @} */

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_DAEMON_H_
