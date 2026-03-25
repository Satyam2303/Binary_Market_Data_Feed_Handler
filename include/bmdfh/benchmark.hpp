#pragma once

#include <cstdint>

namespace bmdfh::benchmark {

[[nodiscard]] inline std::uint64_t read_tsc() {
#if defined(__x86_64__) || defined(__i386__)
  return static_cast<std::uint64_t>(__builtin_ia32_rdtsc());
#else
#error "Phase 8 benchmarking currently requires an x86 TSC source"
#endif
}

}  // namespace bmdfh::benchmark
