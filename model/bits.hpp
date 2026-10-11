#pragma once
// Sign extension and arithmetic right shift written with unsigned operations only.

#include <cstdint>

namespace cutoff {

// Sign-extends the low `bits` bits of v (1 <= bits <= 32) to 32 bits.
constexpr std::uint32_t sext(std::uint32_t v, unsigned bits) {
  const std::uint32_t sign = 1u << (bits - 1);
  const std::uint32_t mask = (sign << 1) - 1;  // all ones when bits == 32
  return ((v & mask) ^ sign) - sign;
}

// Arithmetic right shift of a 32-bit value by s in [0, 31].
constexpr std::uint32_t sra(std::uint32_t a, unsigned s) {
  if (s == 0) return a;
  const std::uint32_t fill = (0u - (a >> 31)) << (32 - s);  // ones in the vacated top bits when a is negative
  return (a >> s) | fill;
}

}  // namespace cutoff
