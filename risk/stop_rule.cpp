#include "stop_rule.hpp"

namespace cutoff::risk {

void StopRule::on_checkpoint(std::uint64_t n, double risk) {
  if (!enabled()) return;
  if (n < cfg_.min_programs) {
    consecutive_ = 0;  // checkpoints before min_programs never count towards the confirmation
    return;
  }
  consecutive_ = risk < cfg_.epsilon ? consecutive_ + 1 : 0;
}

}  // namespace cutoff::risk
