#pragma once
// Deterministic random numbers that do not depend on the standard library's distributions
// (those are implementation-defined, so a seed would give different programs on different platforms).
// SplitMix64 seeds xoshiro256**; bounded draws use Lemire's unbiased method on 32-bit words.

#include <array>
#include <cstdint>

namespace cutoff::gen {

inline constexpr std::uint64_t kSplitMixGamma = 0x9e3779b97f4a7c15ULL;

constexpr std::uint64_t splitmix_mix(std::uint64_t z) {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

// Seed of program number `index` in a campaign started with `campaign_seed`; random access, no state.
constexpr std::uint64_t program_seed(std::uint64_t campaign_seed, std::uint64_t index) {
  return splitmix_mix(splitmix_mix(campaign_seed) + (index + 1) * kSplitMixGamma);
}

class Rng {
 public:
  explicit constexpr Rng(std::uint64_t seed) {
    std::uint64_t sm = seed;
    for (auto& word : s_) {
      sm += kSplitMixGamma;
      word = splitmix_mix(sm);
    }
  }

  constexpr std::uint64_t next64() {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  constexpr std::uint32_t next32() { return static_cast<std::uint32_t>(next64() >> 32); }

  // Uniform in [0, n), n >= 1, without modulo bias.
  constexpr std::uint32_t below(std::uint32_t n) {
    std::uint64_t m = static_cast<std::uint64_t>(next32()) * n;
    auto low = static_cast<std::uint32_t>(m);
    if (low < n) {
      const std::uint32_t threshold = (0u - n) % n;
      while (low < threshold) {
        m = static_cast<std::uint64_t>(next32()) * n;
        low = static_cast<std::uint32_t>(m);
      }
    }
    return static_cast<std::uint32_t>(m >> 32);
  }

 private:
  static constexpr std::uint64_t rotl(std::uint64_t x, unsigned k) { return (x << k) | (x >> (64 - k)); }
  std::array<std::uint64_t, 4> s_{};
};

}  // namespace cutoff::gen
