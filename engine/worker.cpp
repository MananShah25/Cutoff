#include "worker.hpp"

#include <unistd.h>

#include <vector>

#include "check.hpp"
#include "protocol.hpp"

namespace cutoff::engine {

void run_worker(const CampaignConfig& cfg, int in_fd, int out_fd) {
  std::vector<char> batch;
  for (;;) {
    Task task;
    if (!read_full(in_fd, &task, sizeof task) || task.count == 0) _exit(0);
    batch.clear();
    for (std::uint32_t i = 0; i < task.count; ++i) {
      const std::uint64_t id = task.first_id + i;
      if (cfg.crash_worker_at >= 0 && id == static_cast<std::uint64_t>(cfg.crash_worker_at)) _exit(9);  // test hook
      encode_frame(evaluate(cfg, id), batch);
    }
    if (!write_full(out_fd, batch.data(), batch.size())) _exit(3);
  }
}

}  // namespace cutoff::engine
