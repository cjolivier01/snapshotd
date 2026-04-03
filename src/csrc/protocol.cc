/** @file
 *  @brief Control-message framing used by both the daemon and its clients.
 */

#include "src/csrc/protocol.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
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
    const ssize_t written = write(fd, cursor, remaining);
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
  const std::uint32_t size = static_cast<std::uint32_t>(payload.size());
  WriteAll(fd, &size, sizeof(size));
  if (!payload.empty()) {
    WriteAll(fd, payload.data(), payload.size());
  }
}

Message ReceiveMessage(int fd) {
  std::uint32_t size = 0;
  ReadAll(fd, &size, sizeof(size));
  std::string payload(size, '\0');
  if (size > 0) {
    ReadAll(fd, payload.data(), payload.size());
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
