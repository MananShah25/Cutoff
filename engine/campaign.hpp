#pragma once
// The orchestrator: forks worker processes, hands out batches of program ids, and delivers the results to a
// sink in program-id order. Because the sink always sees the same sequence, a campaign's results do not depend
// on the number of workers or on how the operating system schedules them. The sink may end the campaign at any
// delivered program (stop rule reached, budget used up, interruption); programs workers had already computed beyond
// that point are discarded, so the end point is also independent of scheduling.

#include <csignal>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "campaign_config.hpp"
#include "protocol.hpp"

namespace cutoff::engine {

class RecordSink {
 public:
  virtual ~RecordSink() = default;
  // Called once per program, ids first_id, first_id + 1, ... in order. Return false to end the campaign here.
  virtual bool on_record(const ProgramResult& result) = 0;
};

// Accepts results in any order and hands them to the sink strictly in id order.
class OrderedDelivery {
 public:
  OrderedDelivery(RecordSink& sink, std::uint64_t first_id) : sink_(sink), next_(first_id) {}
  // False (and the result is dropped) if the id was already delivered or is already waiting: a worker bug.
  // After the sink has asked to stop, further results are accepted and ignored.
  bool push(const ProgramResult& result);
  std::uint64_t next_id() const { return next_; }  // the id after the last delivered one
  std::size_t waiting() const { return waiting_.size(); }
  bool stopped_by_sink() const { return stopped_; }

 private:
  RecordSink& sink_;
  std::uint64_t next_;
  bool stopped_ = false;
  std::map<std::uint64_t, ProgramResult> waiting_;
};

struct CampaignControl {
  std::uint64_t first_id = 0;  // where to start (non-zero when resuming)
  // Polled about every 200 ms; when it becomes non-zero the campaign winds down cleanly.
  const volatile std::sig_atomic_t* interrupt = nullptr;
};

struct CampaignStatus {
  bool ok = false;                 // ran to a planned end: budget reached, or the sink stopped it
  std::string error;               // what went wrong and what to do next
  bool stopped_by_sink = false;
  bool interrupted = false;
  std::uint64_t next_id = 0;       // id after the last delivered program
};

// `on_progress` (optional) is called about every 200 ms with the id after the last delivered program.
CampaignStatus run_campaign(const CampaignConfig& cfg, RecordSink& sink, const CampaignControl& control = {},
                            const std::function<void(std::uint64_t)>& on_progress = {});

}  // namespace cutoff::engine
