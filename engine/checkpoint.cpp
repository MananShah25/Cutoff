#include "checkpoint.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace cutoff::engine {

namespace {

constexpr char kMagic[8] = {'C', 'U', 'T', 'O', 'F', 'F', 'C', 'K'};
constexpr std::uint32_t kVersion = 3;  // C2 adds StoreMismatch to Totals::by_verdict.

std::uint64_t fnv(const char* data, std::size_t n) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= static_cast<unsigned char>(data[i]);
    h *= 0x100000001b3ULL;
  }
  return h;
}

struct Out {
  std::string bytes;
  template <typename T> void put(const T& v) { bytes.append(reinterpret_cast<const char*>(&v), sizeof v); }
};

struct In {
  const std::string& bytes;
  std::size_t pos = 0;
  bool ok = true;
  template <typename T> T get() {
    T v{};
    if (pos + sizeof v > bytes.size()) { ok = false; return v; }
    std::memcpy(&v, bytes.data() + pos, sizeof v);
    pos += sizeof v;
    return v;
  }
};

}  // namespace

bool write_checkpoint(const std::string& path, const Checkpoint& cp, std::string& error) {
  Out o;
  o.bytes.append(kMagic, sizeof kMagic);
  o.put(kVersion);
  o.put(cp.stream_hash);
  o.put(cp.next_program_id);
  o.put(cp.events_bytes);
  o.put(cp.curve_bytes);
  o.put(cp.stop_consecutive);
  o.put(cp.plateau_stop_at);
  o.put(cp.elapsed_seconds);
  o.put(cp.sessions);
  o.put(cp.totals.programs);
  o.put(cp.totals.failures);
  for (const std::uint64_t v : cp.totals.by_verdict) o.put(v);
  o.put(cp.totals.core_cycles);
  o.put(cp.totals.model_instructions);
  o.put(cp.totals.failure_dirs_written);
  o.put(static_cast<std::uint32_t>(cp.sets.size()));
  for (const SetState& s : cp.sets) {
    o.put(s.set);
    o.put(s.last_new_n);
    o.put(static_cast<std::uint32_t>(s.per_species.size()));
    for (const std::uint32_t y : s.per_species) o.put(y);
  }
  o.put(static_cast<std::uint8_t>(cp.has_drill ? 1 : 0));
  if (cp.has_drill) {
    o.put(cp.drill_stop_at);
    o.put(static_cast<std::uint32_t>(cp.drill_ids.size()));
    for (const int id : cp.drill_ids) o.put(static_cast<std::int32_t>(id));
    o.put(static_cast<std::uint32_t>(cp.drill_words.size()));
    for (const std::uint64_t w : cp.drill_words) o.put(w);
  }
  o.put(fnv(o.bytes.data(), o.bytes.size()));

  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(o.bytes.data(), static_cast<std::streamsize>(o.bytes.size()));
    f.flush();
    if (!f) {
      error = "cannot write " + tmp + ". Check free disk space and permissions.";
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    error = "cannot replace " + path + ": " + ec.message();
    return false;
  }
  return true;
}

bool read_checkpoint(const std::string& path, Checkpoint& cp, std::string& error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    error = "no checkpoint at " + path + ". Only a run folder written by `cutoff run` can be resumed.";
    return false;
  }
  const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const std::string damaged = path + " is damaged or incomplete (the run may have been killed while writing it); it cannot be resumed.";
  if (bytes.size() < sizeof kMagic + 8 || std::memcmp(bytes.data(), kMagic, sizeof kMagic) != 0) { error = path + " is not a Cutoff checkpoint."; return false; }
  std::uint64_t stored = 0;
  std::memcpy(&stored, bytes.data() + bytes.size() - 8, 8);
  if (fnv(bytes.data(), bytes.size() - 8) != stored) { error = damaged; return false; }

  In in{bytes};
  in.pos = sizeof kMagic;
  if (in.get<std::uint32_t>() != kVersion) { error = path + " was written by a different Cutoff version and cannot be resumed."; return false; }
  Checkpoint c;
  c.stream_hash = in.get<std::uint64_t>();
  c.next_program_id = in.get<std::uint64_t>();
  c.events_bytes = in.get<std::uint64_t>();
  c.curve_bytes = in.get<std::uint64_t>();
  c.stop_consecutive = in.get<std::uint32_t>();
  c.plateau_stop_at = in.get<std::uint64_t>();
  c.elapsed_seconds = in.get<double>();
  c.sessions = in.get<std::uint32_t>();
  c.totals.programs = in.get<std::uint64_t>();
  c.totals.failures = in.get<std::uint64_t>();
  for (auto& v : c.totals.by_verdict) v = in.get<std::uint64_t>();
  c.totals.core_cycles = in.get<std::uint64_t>();
  c.totals.model_instructions = in.get<std::uint64_t>();
  c.totals.failure_dirs_written = in.get<std::uint32_t>();
  const auto nsets = in.get<std::uint32_t>();
  for (std::uint32_t i = 0; i < nsets && in.ok; ++i) {
    SetState s;
    s.set = in.get<std::uint8_t>();
    s.last_new_n = in.get<std::uint64_t>();
    const auto n = in.get<std::uint32_t>();
    if (n > 1'000'000) { in.ok = false; break; }
    s.per_species.resize(n);
    for (auto& y : s.per_species) y = in.get<std::uint32_t>();
    c.sets.push_back(std::move(s));
  }
  c.has_drill = in.get<std::uint8_t>() != 0;
  if (c.has_drill) {
    c.drill_stop_at = in.get<std::uint64_t>();
    const auto nids = in.get<std::uint32_t>();
    if (nids > 100000) in.ok = false;
    for (std::uint32_t i = 0; i < nids && in.ok; ++i) c.drill_ids.push_back(in.get<std::int32_t>());
    const auto nwords = in.get<std::uint32_t>();
    if (nwords > 10'000'000) in.ok = false;
    for (std::uint32_t i = 0; i < nwords && in.ok; ++i) c.drill_words.push_back(in.get<std::uint64_t>());
  }
  if (!in.ok || in.pos + 8 != bytes.size()) { error = damaged; return false; }
  cp = std::move(c);
  return true;
}

}  // namespace cutoff::engine
