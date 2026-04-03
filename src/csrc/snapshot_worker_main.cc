/** @file
 *  @brief Small wrapper that reports worker failures on stderr.
 */

#include <iostream>
#include <stdexcept>

#include "src/csrc/worker.h"

int main(int argc, char** argv) {
  try {
    return snapshotd::RunWorkerMain(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
