#include "worker.hpp"

#include <unistd.h>

#include <vector>

#include "check.hpp"
#include "protocol.hpp"

namespace cutoff::engine {

void run_worker(const CampaignConfig& cfg, int in_fd, int out_fd) {
  std::vector<EventRecord> batch;
  for (;;) {
    Task task;
    if (!read_full(in_fd, &task, sizeof task) || task.count == 0) _exit(0);
    batch.clear();
    batch.reserve(task.count);
    for (std::uint32_t i = 0; i < task.count; ++i) {
      const std::uint64_t id = task.first_id + i;
      if (cfg.crash_worker_at >= 0 && id == static_cast<std::uint64_t>(cfg.crash_worker_at)) _exit(9);  // test hook
      batch.push_back(evaluate_program(cfg, id));
    }
    if (!write_full(out_fd, batch.data(), batch.size() * sizeof(EventRecord))) _exit(3);
  }
}

}  // namespace cutoff::engine
