/** @file
 *  @brief CLI entry point for the unprivileged snapshotd client.
 */

#include "src/csrc/client.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/csrc/protocol.h"
#include "src/csrc/util.h"

namespace {

using snapshotd::Client;
using snapshotd::Message;

void PrintUsage() {
  std::cerr
      << "usage: snapshotctl [--socket-path PATH] <run|status|checkpoint|restore> ...\n"
      << "  restore [--namespace-restore] <job-id> [checkpoint-id]\n";
}

void PrintResponse(const Message& response) {
  // The CLI prints a stable key=value format so shell scripts can parse the
  // control-plane responses without needing a JSON dependency.
  for (const auto& field : response.fields) {
    std::cout << field.first << "=" << field.second << "\n";
  }
}

Message CheckedRequest(const Client& client, const Message& request) {
  Message response = client.Request(request);
  if (response.command == "error") {
    throw std::runtime_error(response.Get("message", "unknown control error"));
  }
  return response;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::string socket_path = "/run/snapshotd.sock";
    int index = 1;
    while (index < argc) {
      const std::string arg = argv[index];
      if (arg == "--socket-path") {
        if (index + 1 >= argc) {
          throw std::runtime_error("missing value for --socket-path");
        }
        socket_path = argv[++index];
        ++index;
        continue;
      }
      break;
    }

    if (index >= argc) {
      PrintUsage();
      return 1;
    }

    const std::string command = argv[index++];
    Client client(socket_path);
    Message request;
    request.command = command;

    if (command == "run") {
      if (index < argc && std::string(argv[index]) == "--") {
        ++index;
      }
      if (index >= argc) {
        throw std::runtime_error("run requires a command after --");
      }
      const std::string executable =
          snapshotd::ResolveExecutable(argv[index], snapshotd::GetEnv("PATH"));
      // The daemon requires an absolute executable path so the privileged side
      // never performs PATH lookups on behalf of the caller.
      request.AddField("cwd", snapshotd::GetCurrentWorkingDirectory());
      request.AddField("arg", executable);
      ++index;
      while (index < argc) {
        request.AddField("arg", argv[index++]);
      }
      PrintResponse(CheckedRequest(client, request));
      return 0;
    }

    if (command == "status" || command == "checkpoint") {
      if (index >= argc) {
        throw std::runtime_error(command + " requires <job-id>");
      }
      request.AddField("job_id", argv[index]);
      PrintResponse(CheckedRequest(client, request));
      return 0;
    }

    if (command == "restore") {
      bool namespace_restore = false;
      while (index < argc && std::string(argv[index]) == "--namespace-restore") {
        namespace_restore = true;
        ++index;
      }
      if (index >= argc) {
        throw std::runtime_error("restore requires <job-id>");
      }
      request.AddField("job_id", argv[index++]);
      if (namespace_restore) {
        request.AddField("namespace_restore", "1");
      }
      if (index < argc) {
        request.AddField("checkpoint_id", argv[index++]);
      }
      if (index != argc) {
        throw std::runtime_error("restore accepts at most one optional checkpoint id");
      }
      PrintResponse(CheckedRequest(client, request));
      return 0;
    }

    throw std::runtime_error("unknown snapshotctl command: " + command);
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
