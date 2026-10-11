#pragma once
// Wire and file formats shared by the orchestrator, the workers and the results files.
// All integers are little-endian (asserted below); records are fixed-size so they can be streamed and mmapped.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "species.hpp"

namespace cutoff::engine {

static_assert(std::endian::native == std::endian::little, "events.bin is defined as little-endian");

// Outcome of checking one program. Anything but Pass is a failure.
enum class Verdict : std::uint8_t {
  Pass = 0,
  GeneratorFault = 1,    // the reference model did not finish the program cleanly: a generator or model bug, not a core bug
  Hang = 2,              // the core hit its cycle limit although the model finished
  StatusMismatch = 3,    // core ended in a different state (trap, memfault) than the model
  ExitCodeMismatch = 4,  // the magic-store data differs
  RegMismatch = 5,       // a final register differs; `detail` is the first differing register
  MemMismatch = 6,       // final memory hash differs
};

const char* to_string(Verdict v);

// One checked program. No wall-clock data lives here, so the event stream of a campaign is identical
// whatever the worker count or timing.
struct EventRecord {
  std::uint64_t program_id = 0;
  std::uint64_t seed = 0;
  std::uint64_t program_hash = 0;    // FNV-1a of the generated code
  std::uint64_t model_reg_hash = 0;  // FNV-1a of the model's final x0..x31
  std::uint64_t model_mem_hash = 0;
  std::uint64_t core_mem_hash = 0;
  std::uint64_t core_cycles = 0;
  std::uint32_t model_instret = 0;
  std::uint32_t model_exit_code = 0;
  std::uint32_t core_exit_code = 0;
  std::uint8_t model_status = 0;  // cutoff::Status
  std::uint8_t core_status = 0;
  std::uint8_t verdict = 0;       // Verdict
  std::uint8_t detail = 0;        // first differing register for RegMismatch
};
static_assert(sizeof(EventRecord) == 72);
static_assert(std::is_trivially_copyable_v<EventRecord>);

// What a worker sends back for one program: the compact record plus the species the program exercised.
struct ProgramResult {
  EventRecord rec;
  risk::SpeciesIds species;
};

// Wire frame: EventRecord, then one little-endian u16 species count per set (kMaxSpeciesSets of them), then the ids of
// each set in order. Only `rec` goes into events.bin; the species feed the estimators and are not stored.
void encode_frame(const ProgramResult& r, std::vector<char>& out);
// Decodes one frame from the front of `data`. Returns the bytes consumed, or 0 if `avail` holds only part of a frame.
std::size_t decode_frame(const char* data, std::size_t avail, ProgramResult& out);

// Orchestrator -> worker. count == 0 tells the worker to exit.
struct Task {
  std::uint64_t first_id = 0;
  std::uint32_t count = 0;
  std::uint32_t reserved = 0;
};
static_assert(sizeof(Task) == 16);

inline constexpr char kEventsMagic[8] = {'C', 'U', 'T', 'O', 'F', 'F', 'E', 'V'};
inline constexpr std::uint32_t kEventsVersion = 1;

struct EventsHeader {
  char magic[8] = {kEventsMagic[0], kEventsMagic[1], kEventsMagic[2], kEventsMagic[3],
                   kEventsMagic[4], kEventsMagic[5], kEventsMagic[6], kEventsMagic[7]};
  std::uint32_t version = kEventsVersion;
  std::uint32_t record_size = sizeof(EventRecord);
  std::uint32_t generator_version = 0;
  std::uint32_t profile = 0;
  std::uint64_t campaign_seed = 0;
  std::uint32_t checker = 0;  // 1 = C1
  std::uint32_t reserved = 0;
};
static_assert(sizeof(EventsHeader) == 40);

// Blocking helpers that retry on EINTR; false on error or (for read_full) on EOF before `n` bytes.
bool write_full(int fd, const void* data, std::size_t n);
bool read_full(int fd, void* data, std::size_t n);

}  // namespace cutoff::engine
