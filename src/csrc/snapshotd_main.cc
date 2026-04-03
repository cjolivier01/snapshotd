#include <iostream>
#include <stdexcept>

#include "src/csrc/daemon.h"

int main(int argc, char** argv) {
  try {
    return snapshotd::RunDaemonMain(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
