#ifndef SNAPSHOT_CSRC_DAEMON_H_
#define SNAPSHOT_CSRC_DAEMON_H_

#include <filesystem>
#include <string>

#include "src/csrc/protocol.h"
#include "src/csrc/store.h"

namespace snapshotd {

struct DaemonConfig {
  std::string socket_path;
  std::filesystem::path state_dir;
  std::string worker_path;
  std::string criu_bin;
  std::string criu_ns_bin;
  std::string ready_file;
};

Message HandleRequest(
    const Message& request,
    const PeerCred& peer,
    const DaemonConfig& config,
    Store* store);

int RunDaemon(const DaemonConfig& config);
int RunDaemonMain(int argc, char** argv);

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_DAEMON_H_
