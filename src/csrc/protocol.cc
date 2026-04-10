/** @file
 *  @brief Control-message framing used by both the daemon and its clients.
 *
 *  @details
 *  This file implements the minimal wire protocol used on the privileged Unix
 *  socket. The framing layer is intentionally small because every accepted byte
 *  is part of the daemon's root attack surface.
 */

#include "src/csrc/protocol.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/csrc/util.h"

namespace snapshotd {

namespace {

void WriteAll(int fd, const void* buffer, std::size_t size) {
  const char* cursor = static_cast<const char*>(buffer);
  std::size_t remaining = size;
  while (remaining > 0) {
    // Use MSG_NOSIGNAL so a disconnected peer turns into EPIPE instead of
    // terminating the whole daemon with SIGPIPE.
    const ssize_t written = send(fd, cursor, remaining, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno("write");
    }
    remaining -= static_cast<std::size_t>(written);
    cursor += written;
  }
}

void ReadAll(int fd, void* buffer, std::size_t size) {
  char* cursor = static_cast<char*>(buffer);
  std::size_t remaining = size;
  while (remaining > 0) {
    const ssize_t count = read(fd, cursor, remaining);
    if (count == 0) {
      throw std::runtime_error("unexpected EOF on control socket");
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno("read");
    }
    remaining -= static_cast<std::size_t>(count);
    cursor += count;
  }
}

std::string EncodeMessage(const Message& message) {
  if (message.command.empty()) {
    throw std::runtime_error("message command may not be empty");
  }
  // The wire format is a length-prefixed sequence of NUL-delimited tokens:
  //   command\0key=value\0key=value\0
  std::string output;
  output.append(message.command);
  output.push_back('\0');
  for (const auto& [key, value] : message.fields) {
    if (key.empty() || key.find('\0') != std::string::npos || value.find('\0') != std::string::npos) {
      throw std::runtime_error("invalid NUL in control message");
    }
    output.append(key);
    output.push_back('=');
    output.append(value);
    output.push_back('\0');
  }
  return output;
}

Message DecodeMessage(const std::string& payload) {
  Message message;
  std::size_t cursor = 0;
  bool first = true;
  while (cursor < payload.size()) {
    const std::size_t end = payload.find('\0', cursor);
    const std::size_t token_end = end == std::string::npos ? payload.size() : end;
    const std::string token = payload.substr(cursor, token_end - cursor);
    cursor = token_end == payload.size() ? payload.size() : token_end + 1;
    if (token.empty()) {
      continue;
    }
    if (first) {
      message.command = token;
      first = false;
      continue;
    }
    const std::size_t delimiter = token.find('=');
    if (delimiter == std::string::npos) {
      throw std::runtime_error("malformed control token");
    }
    message.fields.emplace_back(token.substr(0, delimiter), token.substr(delimiter + 1));
  }
  if (message.command.empty()) {
    throw std::runtime_error("missing control command");
  }
  return message;
}

}  // namespace

void Message::AddField(const std::string& key, const std::string& value) {
  fields.emplace_back(key, value);
}

std::string Message::Get(const std::string& key, const std::string& default_value) const {
  for (const auto& [current_key, value] : fields) {
    if (current_key == key) {
      return value;
    }
  }
  return default_value;
}

std::vector<std::string> Message::GetAll(const std::string& key) const {
  std::vector<std::string> values;
  for (const auto& [current_key, value] : fields) {
    if (current_key == key) {
      values.push_back(value);
    }
  }
  return values;
}

PeerCred GetPeerCred(int fd) {
  struct ucred cred {};
  socklen_t length = sizeof(cred);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &length) != 0) {
    ThrowErrno("getsockopt(SO_PEERCRED)");
  }
  PeerCred peer;
  peer.pid = cred.pid;
  peer.uid = cred.uid;
  peer.gid = cred.gid;
  return peer;
}

void SendMessage(int fd, const Message& message) {
  // Prefix the payload so the receiver can safely read one whole message from a
  // stream socket without guessing message boundaries.
  const std::string payload = EncodeMessage(message);
  if (payload.size() > kMaxControlMessageBytes) {
    throw std::runtime_error("control message exceeds maximum payload size");
  }
  const std::uint32_t size = static_cast<std::uint32_t>(payload.size());
  WriteAll(fd, &size, sizeof(size));
  if (!payload.empty()) {
    WriteAll(fd, payload.data(), payload.size());
  }
}

Message ReceiveMessage(int fd) {
  std::uint32_t size = 0;
  ReadAll(fd, &size, sizeof(size));
  if (size > kMaxControlMessageBytes) {
    throw std::runtime_error("control message exceeds maximum payload size");
  }
  std::string payload(size, '\0');
  if (size > 0) {
    ReadAll(fd, payload.data(), payload.size());
  }
  return DecodeMessage(payload);
}

void SendMessageWithFd(int socket_fd, const Message& message, int ancillary_fd) {
  const std::string payload = EncodeMessage(message);
  if (payload.size() > kMaxControlMessageBytes) {
    throw std::runtime_error("control message exceeds maximum payload size");
  }
  // Build a contiguous header+payload buffer so the fd is attached to a
  // single sendmsg call.  This keeps the ancillary data associated with the
  // first byte of the framed message and avoids splitting across calls.
  const std::uint32_t wire_size = static_cast<std::uint32_t>(payload.size());
  std::string wire(sizeof(wire_size) + payload.size(), '\0');
  std::memcpy(wire.data(), &wire_size, sizeof(wire_size));
  std::memcpy(wire.data() + sizeof(wire_size), payload.data(), payload.size());

  struct iovec iov {};
  iov.iov_base = wire.data();
  iov.iov_len = wire.size();

  struct msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  // Ancillary-data buffer — sized for one int (the file descriptor).
  union {
    struct cmsghdr align;
    char buf[CMSG_SPACE(sizeof(int))];
  } cmsg_buf;

  if (ancillary_fd >= 0) {
    std::memset(&cmsg_buf, 0, sizeof(cmsg_buf));
    msg.msg_control = cmsg_buf.buf;
    msg.msg_controllen = sizeof(cmsg_buf.buf);
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &ancillary_fd, sizeof(int));
  }

  std::size_t total_sent = 0;
  while (total_sent < wire.size()) {
    // Only attach the ancillary data on the first sendmsg call.
    if (total_sent > 0) {
      msg.msg_control = nullptr;
      msg.msg_controllen = 0;
    }
    iov.iov_base = wire.data() + total_sent;
    iov.iov_len = wire.size() - total_sent;
    const ssize_t sent = sendmsg(socket_fd, &msg, MSG_NOSIGNAL);
    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno("sendmsg");
    }
    total_sent += static_cast<std::size_t>(sent);
  }
}

Message ReceiveMessageWithFd(int socket_fd, int* received_fd) {
  *received_fd = -1;

  // Read the 4-byte size header via recvmsg so we can receive the ancillary
  // fd that is attached to the first byte of the stream.
  std::uint32_t wire_size = 0;
  {
    struct iovec iov {};
    iov.iov_base = &wire_size;
    iov.iov_len = sizeof(wire_size);

    union {
      struct cmsghdr align;
      char buf[CMSG_SPACE(sizeof(int))];
    } cmsg_buf;
    std::memset(&cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf.buf;
    msg.msg_controllen = sizeof(cmsg_buf.buf);

    std::size_t total_read = 0;
    while (total_read < sizeof(wire_size)) {
      iov.iov_base = reinterpret_cast<char*>(&wire_size) + total_read;
      iov.iov_len = sizeof(wire_size) - total_read;
      // Only look for ancillary data on the first recvmsg call.
      if (total_read > 0) {
        msg.msg_control = nullptr;
        msg.msg_controllen = 0;
      }
      const ssize_t count = recvmsg(socket_fd, &msg, 0);
      if (count == 0) {
        throw std::runtime_error("unexpected EOF on control socket");
      }
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        ThrowErrno("recvmsg");
      }
      total_read += static_cast<std::size_t>(count);

      // Extract the ancillary fd from the first successful recvmsg.
      if (*received_fd < 0) {
        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
          if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
              cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            std::memcpy(received_fd, CMSG_DATA(cmsg), sizeof(int));
          }
        }
      }
    }
  }

  if (wire_size > kMaxControlMessageBytes) {
    if (*received_fd >= 0) {
      close(*received_fd);
      *received_fd = -1;
    }
    throw std::runtime_error("control message exceeds maximum payload size");
  }
  std::string payload(wire_size, '\0');
  if (wire_size > 0) {
    ReadAll(socket_fd, payload.data(), payload.size());
  }
  return DecodeMessage(payload);
}

void ValidateAllowedFields(
    const Message& message,
    const std::unordered_set<std::string>& allowed_fields) {
  for (const auto& [key, _value] : message.fields) {
    if (allowed_fields.find(key) == allowed_fields.end()) {
      throw std::runtime_error("unexpected field in request: " + key);
    }
  }
}

}  // namespace snapshotd
