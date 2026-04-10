#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int Usage() {
  std::cerr << "usage: restore_stub --pidfile PATH\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 || std::string(argv[1]) != "--pidfile") {
    return Usage();
  }

  const pid_t child = fork();
  if (child < 0) {
    std::perror("fork");
    return 1;
  }

  if (child == 0) {
    struct sigaction ignore_action {};
    ignore_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_action.sa_mask);
    (void)!sigaction(SIGHUP, &ignore_action, nullptr);
    (void)!setsid();
    for (;;) {
      sleep(60);
    }
  }

  std::ofstream output(argv[2], std::ios::out | std::ios::trunc);
  if (!output.is_open()) {
    std::cerr << "failed to open pidfile: " << argv[2] << "\n";
    return 1;
  }
  output << child << "\n";
  return output.good() ? 0 : 1;
}
