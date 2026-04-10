/** @file
 *  @brief Entry points for the short-lived privileged CRIU worker.
 */

#ifndef SNAPSHOT_CSRC_WORKER_H_
#define SNAPSHOT_CSRC_WORKER_H_

#include <sys/types.h>

#include <filesystem>
#include <string>
#include <vector>

namespace snapshotd {

/**
 * @defgroup worker_api CRIU Worker
 * @brief Short-lived worker that executes fixed CRIU dump/restore commands.
 *
 * The worker is intentionally not a public API surface. It receives
 * fully-resolved, broker-owned paths from the daemon and converts them into a
 * small fixed set of CRIU invocations.
 *
 * @see @ref daemon_api
 * @see @ref safe_root_criu_broker_design
 * @{
 */

/** @brief Fully-resolved worker configuration for one dump or restore request. */
struct WorkerConfig {
  /** @brief Either `dump` or `restore`. */
  std::string operation;
  /** @brief Root-owned state root for path confinement checks. */
  std::filesystem::path state_dir;
  /** @brief Private job directory beneath @ref state_dir. */
  std::filesystem::path job_dir;
  /** @brief Private checkpoint directory beneath @ref job_dir. */
  std::filesystem::path checkpoint_dir;
  /** @brief Absolute path to the CRIU binary. */
  std::string criu_bin;
  /** @brief Absolute path to the pid-namespace helper binary. */
  std::string criu_ns_bin;
  /** @brief Target pid for dump operations. */
  pid_t pid = 0;
  /** @brief Whether dump should occur from a broker-created pid namespace. */
  bool namespace_dump = false;
  /** @brief Whether restore should occur inside a broker-created pid namespace. */
  bool namespace_restore = false;
  /** @brief Small allowlisted set of extra CRIU flags forwarded by the daemon. */
  std::vector<std::string> extra_args;
};

/** @brief Construct the exact CRIU command used for a dump operation. */
std::vector<std::string> BuildDumpCommand(const WorkerConfig& config);
/** @brief Construct the exact CRIU command used for a restore operation. */
std::vector<std::string> BuildRestoreCommand(const WorkerConfig& config);
/** @brief Execute the requested dump or restore operation. */
int RunWorker(const WorkerConfig& config);
/** @brief Parse worker CLI flags and invoke @ref RunWorker. */
int RunWorkerMain(int argc, char** argv);

/** @} */

}  // namespace snapshotd

#endif  // SNAPSHOT_CSRC_WORKER_H_
