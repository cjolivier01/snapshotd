/** @file
 *  @brief Implementation of the synchronous control-socket client.
 */

#include "src/csrc/client.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

#include "src/csrc/util.h"

namespace snapshotd {

Client::Client(std::string socket_path) : socket_path_(std::move(socket_path)) {}

Message Client::Request(const Message& request) const {
  // Each request uses a fresh Unix-domain socket so the CLI never needs to keep
  // a long-lived privileged channel open.
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    ThrowErrno("socket(AF_UNIX)");
  }

  sockaddr_un address {};
  address.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(address.sun_path)) {
    close(fd);
    throw std::runtime_error("socket path too long");
  }
  std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path_.c_str());

  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string error = ErrnoMessage("connect(" + socket_path_ + ")");
    close(fd);
    throw std::runtime_error(error);
  }

  try {
    SendMessage(fd, request);
    Message response = ReceiveMessage(fd);
    close(fd);
    return response;
  } catch (...) {
    close(fd);
    throw;
  }
}

}  // namespace snapshotd
