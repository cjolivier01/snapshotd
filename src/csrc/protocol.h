#ifndef SNAPSHOT_CSRC_PROTOCOL_H_
#define SNAPSHOT_CSRC_PROTOCOL_H_

#include <sys/types.h>

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace snapshotd {

struct PeerCred {
  pid_t pid = 0;
  uid_t uid = 0;
  gid_t gid = 0;
};

struct Message {
  std::string command;
  std::vector<std::pair<std::string, std::string>> fields;

  void AddField(const std::string& key, const std::string& value);
  std::string Get(const std::string& key, const std::string& default_value = "") const;
  std::vector<std::string> GetAll(const std::string& key) const;
};

PeerCred GetPeerCred(int fd);
void SendMessage(int fd, const Message& message);
Message ReceiveMessage(int fd);
void ValidateAllowedFields(
    const Message& message,
    const std::unordered_set<std::string>& allowed_fields);

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_PROTOCOL_H_
