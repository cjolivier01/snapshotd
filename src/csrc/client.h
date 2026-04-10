/** @file
 *  @brief Thin client wrapper for the snapshotd control socket.
 */

#ifndef SNAPSHOT_CSRC_CLIENT_H_
#define SNAPSHOT_CSRC_CLIENT_H_

#include <string>

#include "src/csrc/protocol.h"

namespace snapshotd {

/**
 * @defgroup client_api Control Client
 * @brief Unprivileged client wrapper used by `snapshotctl` and tests.
 *
 * The client uses a fresh Unix-domain socket connection per request. That keeps
 * the caller-side implementation simple and avoids long-lived privileged
 * channels outside the broker itself.
 *
 * @see @ref protocol_api
 * @see @ref safe_root_criu_broker_design
 * @{
 */

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

  /**
   * @brief Send a request with an ancillary file descriptor.
   *
   * The descriptor is transmitted via SCM_RIGHTS so the daemon receives a
   * kernel-duplicated copy.  The caller retains ownership of @p ancillary_fd.
   *
   * @param request Fully-populated control message.
   * @param ancillary_fd File descriptor to pass (-1 to skip).
   * @return The daemon response message.
   */
  Message RequestWithFd(const Message& request, int ancillary_fd) const;

 private:
  std::string socket_path_;
};

/** @} */

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_CLIENT_H_
