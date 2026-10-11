#pragma once
// The orchestrator: forks worker processes, hands out batches of program ids, and delivers the results to a
// sink in program-id order. Because the sink always sees the same sequence, a campaign's results do not depend
// on the number of workers or on how the operating system schedules them.

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
  virtual void on_record(const EventRecord& rec) = 0;  // called once per program, ids 0, 1, 2, ... in order
};

// Accepts records in any order and hands them to the sink strictly as ids 0, 1, 2, ...
class OrderedDelivery {
 public:
  explicit OrderedDelivery(RecordSink& sink) : sink_(sink) {}
  // False (and the record is dropped) if the id was already delivered or is already waiting: a worker bug.
  bool push(const EventRecord& rec);
  std::uint64_t delivered() const { return next_; }
  std::size_t waiting() const { return waiting_.size(); }

 private:
  RecordSink& sink_;
  std::uint64_t next_ = 0;
  std::map<std::uint64_t, EventRecord> waiting_;
};

struct CampaignStatus {
  bool ok = false;
  std::string error;               // what went wrong and what to do next
  std::uint64_t programs_done = 0; // records delivered to the sink
};

// `on_progress` (optional) is called about every 200 ms from the orchestrator with the number delivered so far.
CampaignStatus run_campaign(const CampaignConfig& cfg, RecordSink& sink, const std::function<void(std::uint64_t)>& on_progress = {});

}  // namespace cutoff::engine
