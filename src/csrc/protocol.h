/** @file
 *  @brief Framing and validation for the snapshotd control protocol.
 */

#ifndef SNAPSHOT_CSRC_PROTOCOL_H_
#define SNAPSHOT_CSRC_PROTOCOL_H_

#include <sys/types.h>

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace snapshotd {

/** @brief Kernel-authenticated Unix-socket peer credentials. */
struct PeerCred {
  pid_t pid = 0;
  uid_t uid = 0;
  gid_t gid = 0;
};

/** @brief Simple key/value control message exchanged over the daemon socket. */
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
/** @brief Reject any request field that is not explicitly allowlisted. */
void ValidateAllowedFields(
    const Message& message,
    const std::unordered_set<std::string>& allowed_fields);

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_PROTOCOL_H_
