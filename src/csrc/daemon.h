/** @file
 *  @brief Public entry points for the privileged snapshotd broker.
 */

#ifndef SNAPSHOT_CSRC_DAEMON_H_
#define SNAPSHOT_CSRC_DAEMON_H_

#include <filesystem>
#include <string>

#include "src/csrc/protocol.h"
#include "src/csrc/store.h"

namespace snapshotd {

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
 * @return Reply message to send back on the same socket.
 */
Message HandleRequest(
    const Message& request,
    const PeerCred& peer,
    const DaemonConfig& config,
    Store* store);

/** @brief Run the daemon event loop until shutdown. */
int RunDaemon(const DaemonConfig& config);
/** @brief Parse CLI flags and invoke @ref RunDaemon. */
int RunDaemonMain(int argc, char** argv);

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_DAEMON_H_
