#include "protocol.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <array>

namespace cutoff::engine {

const char* to_string(Verdict v) {
  switch (v) {
    case Verdict::Pass: return "pass";
    case Verdict::GeneratorFault: return "generator-fault";
    case Verdict::Hang: return "hang";
    case Verdict::StatusMismatch: return "status-mismatch";
    case Verdict::ExitCodeMismatch: return "exit-code-mismatch";
    case Verdict::RegMismatch: return "register-mismatch";
    case Verdict::MemMismatch: return "memory-mismatch";
  }
  return "?";
}

void encode_frame(const ProgramResult& r, std::vector<char>& out) {
  const std::size_t start = out.size();
  std::size_t total = sizeof(EventRecord) + 2 * risk::kMaxSpeciesSets;
  for (const auto& ids : r.species) total += 2 * ids.size();
  out.resize(start + total);
  char* p = out.data() + start;
  std::memcpy(p, &r.rec, sizeof r.rec);
  p += sizeof r.rec;
  for (const auto& ids : r.species) {
    const auto n = static_cast<std::uint16_t>(ids.size());
    std::memcpy(p, &n, 2);
    p += 2;
  }
  for (const auto& ids : r.species) {
    if (!ids.empty()) std::memcpy(p, ids.data(), 2 * ids.size());
    p += 2 * ids.size();
  }
}

std::size_t decode_frame(const char* data, std::size_t avail, ProgramResult& out) {
  constexpr std::size_t head = sizeof(EventRecord) + 2 * risk::kMaxSpeciesSets;
  if (avail < head) return 0;
  std::array<std::uint16_t, risk::kMaxSpeciesSets> counts{};
  std::size_t body = 0;
  for (std::size_t i = 0; i < risk::kMaxSpeciesSets; ++i) {
    std::memcpy(&counts[i], data + sizeof(EventRecord) + 2 * i, 2);
    body += 2 * static_cast<std::size_t>(counts[i]);
  }
  if (avail < head + body) return 0;
  std::memcpy(&out.rec, data, sizeof out.rec);
  const char* p = data + head;
  for (std::size_t i = 0; i < risk::kMaxSpeciesSets; ++i) {
    out.species[i].resize(counts[i]);
    if (counts[i] != 0) std::memcpy(out.species[i].data(), p, 2 * static_cast<std::size_t>(counts[i]));
    p += 2 * static_cast<std::size_t>(counts[i]);
  }
  return head + body;
}

bool write_full(int fd, const void* data, std::size_t n) {
  const auto* p = static_cast<const char*>(data);
  while (n > 0) {
    const ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= static_cast<std::size_t>(w);
  }
  return true;
}

bool read_full(int fd, void* data, std::size_t n) {
  auto* p = static_cast<char*>(data);
  while (n > 0) {
    const ssize_t r = ::read(fd, p, n);
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;
    p += r;
    n -= static_cast<std::size_t>(r);
  }
  return true;
}

}  // namespace cutoff::engine
