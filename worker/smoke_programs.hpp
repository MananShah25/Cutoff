#pragma once
// Hand-assembled programs for the M0 smoke tests. Encodings were written by hand
// (not by an assembler) so they stay independent of the Step 3 encoder.

#include <array>
#include <cstdint>

namespace cutoff::smoke {

// x1=5; x2=7; x3=x1+x2; store x3 to the magic address. Expected exit code 12.
inline constexpr std::array<std::uint32_t, 6> kAdd = {
    0x00500093,  // addi x1, x0, 5
    0x00700113,  // addi x2, x0, 7
    0x002081b3,  // add  x3, x1, x2
    0x10000237,  // lui  x4, 0x10000
    0x00322023,  // sw   x3, 0(x4)
    0x0000006f,  // jal  x0, 0   (spin; never reached)
};

// JALR to an odd target: the spec clears bit 0, so control must land on 0x10.
// x2 gets the link address 8; the skipped addi would set x5 to 99. Expected exit code 8, x5 == 0.
inline constexpr std::array<std::uint32_t, 7> kJalrOdd = {
    0x01100093,  // 0x00 addi x1, x0, 17   (0x10 | 1)
    0x00008167,  // 0x04 jalr x2, 0(x1)
    0x06300293,  // 0x08 addi x5, x0, 99   (must be skipped)
    0x0000006f,  // 0x0c jal  x0, 0
    0x10000237,  // 0x10 lui  x4, 0x10000
    0x00222023,  // 0x14 sw   x2, 0(x4)
    0x0000006f,  // 0x18 jal  x0, 0
};

}  // namespace cutoff::smoke
