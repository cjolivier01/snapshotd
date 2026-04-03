#ifndef SNAPSHOT_CSRC_CLIENT_H_
#define SNAPSHOT_CSRC_CLIENT_H_

#include <string>

#include "src/csrc/protocol.h"

namespace snapshotd {

class Client {
 public:
  explicit Client(std::string socket_path);

  Message Request(const Message& request) const;

 private:
  std::string socket_path_;
};

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_CLIENT_H_
