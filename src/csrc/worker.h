#ifndef SNAPSHOT_CSRC_WORKER_H_
#define SNAPSHOT_CSRC_WORKER_H_

#include <sys/types.h>

#include <filesystem>
#include <string>
#include <vector>

namespace snapshotd {

struct WorkerConfig {
  std::string operation;
  std::filesystem::path state_dir;
  std::filesystem::path job_dir;
  std::filesystem::path checkpoint_dir;
  std::string criu_bin;
  std::string criu_ns_bin;
  pid_t pid = 0;
  bool namespace_dump = false;
  bool namespace_restore = false;
  std::vector<std::string> extra_args;
};

std::vector<std::string> BuildDumpCommand(const WorkerConfig& config);
std::vector<std::string> BuildRestoreCommand(const WorkerConfig& config);
int RunWorker(const WorkerConfig& config);
int RunWorkerMain(int argc, char** argv);

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_WORKER_H_
