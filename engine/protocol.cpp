#include "protocol.hpp"

#include <cerrno>
#include <unistd.h>

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
