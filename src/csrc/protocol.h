/** @file
 *  @brief Framing and validation for the snapshotd control protocol.
 */

#ifndef SNAPSHOT_CSRC_PROTOCOL_H_
#define SNAPSHOT_CSRC_PROTOCOL_H_

#include <cstddef>
#include <sys/types.h>

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace snapshotd {

/**
 * @defgroup protocol_api Control Protocol
 * @brief Length-prefixed request/response protocol used on the daemon socket.
 *
 * These types and helpers define the only wire format accepted by the
 * privileged broker. The protocol is intentionally small and field-allowlisted
 * so the root daemon never exposes a raw CRIU command tunnel.
 *
 * @see @ref safe_root_criu_broker_design
 * @{
 */

/**
 * @brief Upper bound for one control-message payload.
 *
 * The daemon is root and accepts messages from any member of the socket group,
 * so the framing layer must cap per-request memory growth before allocating a
 * payload buffer.
 */
inline constexpr std::size_t kMaxControlMessageBytes = 64 * 1024;

/** @brief Kernel-authenticated Unix-socket peer credentials. */
struct PeerCred {
  pid_t pid = 0;
  uid_t uid = 0;
  gid_t gid = 0;
};

/**
 * @brief Simple key/value control message exchanged over the daemon socket.
 *
 * The protocol intentionally uses repeated string fields instead of a
 * general-purpose object format. That keeps the root daemon's parsing surface
 * small, deterministic, and easy to audit in code review.
 */
struct Message {
  /** @brief Command verb such as `run`, `checkpoint`, or `error`. */
  std::string command;
  /** @brief Repeated string fields associated with the command. */
  std::vector<std::pair<std::string, std::string>> fields;

  /** @brief Append a new field while preserving insertion order. */
  void AddField(const std::string& key, const std::string& value);
  /** @brief Return the first value for @p key, or @p default_value if absent. */
  std::string Get(const std::string& key, const std::string& default_value = "") const;
  /** @brief Return all values associated with @p key. */
  std::vector<std::string> GetAll(const std::string& key) const;
};

/** @brief Read SO_PEERCRED from a connected Unix-domain socket. */
PeerCred GetPeerCred(int fd);
/** @brief Encode and send one framed control message. */
void SendMessage(int fd, const Message& message);
/** @brief Receive and decode one framed control message. */
Message ReceiveMessage(int fd);

/**
 * @brief Send a framed control message with an optional file descriptor.
 *
 * When @p ancillary_fd >= 0 the descriptor is transmitted to the peer via
 * SCM_RIGHTS ancillary data on the first sendmsg(2) call.  The caller
 * retains ownership of the descriptor (it is not closed).
 */
void SendMessageWithFd(int socket_fd, const Message& message, int ancillary_fd);

/**
 * @brief Receive a framed control message and an optional file descriptor.
 *
 * If the peer attached a descriptor via SCM_RIGHTS, @p *received_fd is set
 * to the new local descriptor and the caller takes ownership (must close).
 * Otherwise @p *received_fd is set to -1.
 */
Message ReceiveMessageWithFd(int socket_fd, int* received_fd);

/** @brief Reject any request field that is not explicitly allowlisted. */
void ValidateAllowedFields(
    const Message& message,
    const std::unordered_set<std::string>& allowed_fields);

/** @} */

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_PROTOCOL_H_
