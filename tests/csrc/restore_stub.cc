#include <signal.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int Usage() {
  std::cerr << "usage: restore_stub --pidfile PATH [--tty-log PATH]\n";
  return 2;
}

std::string ReadSymlinkTarget(const std::string& path) {
  std::array<char, 4096> buffer {};
  const ssize_t count = readlink(path.c_str(), buffer.data(), buffer.size() - 1);
  if (count < 0) {
    return "unavailable";
  }
  buffer[static_cast<std::size_t>(count)] = '\0';
  return std::string(buffer.data());
}

void WriteTtyStateLog(const std::string& tty_log_path) {
  if (tty_log_path.empty()) {
    return;
  }
  std::ifstream stat_input("/proc/self/stat");
  if (!stat_input.is_open()) {
    std::cerr << "failed to open /proc/self/stat\n";
    _exit(1);
  }
  std::string pid_field;
  std::string comm_field;
  std::string state_field;
  std::string ppid_field;
  std::string pgrp_field;
  std::string sid_field;
  std::string tty_nr_field;
  std::string tpgid_field;
  if (!(stat_input >> pid_field >> comm_field >> state_field >> ppid_field >> pgrp_field >>
        sid_field >> tty_nr_field >> tpgid_field)) {
    std::cerr << "failed to parse /proc/self/stat\n";
    _exit(1);
  }

  std::ofstream output(tty_log_path, std::ios::out | std::ios::trunc);
  if (!output.is_open()) {
    std::cerr << "failed to open tty log: " << tty_log_path << "\n";
    _exit(1);
  }
  output << "pid=" << pid_field << "\n";
  output << "pgrp=" << pgrp_field << "\n";
  output << "sid=" << sid_field << "\n";
  output << "tty_nr=" << tty_nr_field << "\n";
  output << "tpgid=" << tpgid_field << "\n";
  output << "tty_path=" << (isatty(STDIN_FILENO) ? ttyname(STDIN_FILENO) : "not a tty") << "\n";
  output << "stdin=" << ReadSymlinkTarget("/proc/self/fd/0") << "\n";
  output << "stdout=" << ReadSymlinkTarget("/proc/self/fd/1") << "\n";
  output << "stderr=" << ReadSymlinkTarget("/proc/self/fd/2") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string pidfile_path;
  std::string tty_log_path;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--pidfile" && index + 1 < argc) {
      pidfile_path = argv[++index];
      continue;
    }
    if (arg == "--tty-log" && index + 1 < argc) {
      tty_log_path = argv[++index];
      continue;
    }
    return Usage();
  }
  if (pidfile_path.empty()) {
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
    WriteTtyStateLog(tty_log_path);
    for (;;) {
      sleep(60);
    }
  }

  std::ofstream output(pidfile_path, std::ios::out | std::ios::trunc);
  if (!output.is_open()) {
    std::cerr << "failed to open pidfile: " << pidfile_path << "\n";
    return 1;
  }
  output << child << "\n";
  return output.good() ? 0 : 1;
}
