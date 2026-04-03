/** @file
 *  @brief Thin client wrapper for the snapshotd control socket.
 */

#ifndef SNAPSHOT_CSRC_CLIENT_H_
#define SNAPSHOT_CSRC_CLIENT_H_

#include <string>

#include "src/csrc/protocol.h"

namespace snapshotd {

/** @brief Synchronous client for one-request/one-response control operations. */
class Client {
 public:
  /** @brief Create a client that talks to the given Unix-domain socket path. */
  explicit Client(std::string socket_path);

  /**
   * @brief Send a request to the daemon and wait for the response.
   *
   * @param request Fully-populated control message.
   * @return The daemon response message.
   */
  Message Request(const Message& request) const;

 private:
  std::string socket_path_;
};

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_CLIENT_H_
