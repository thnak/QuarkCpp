// Implements shared build/config primitives used across the core.
// Cross-platform seams live in pal/ (019); this header is std-only and portable.
#pragma once

#include <cstddef>
#include <new>

namespace quark {

// Cache-line size for alignment / false-sharing avoidance (003, 023).
// x86-64 is 64 B; std::hardware_destructive_interference_size is the portable source
// but is not guaranteed present, so we pin the verified value for the primary target.
#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t cache_line_size = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t cache_line_size = 64;
#endif

// Descriptor + handle hard ceiling (003): one cache line.
inline constexpr std::size_t max_descriptor_size = 64;

}  // namespace quark

// Branch-prediction + inlining hints (hot path, 023). Kept as macros so they read the same
// at every call site; no behavioural change, only codegen.
#if defined(__GNUC__) || defined(__clang__)
#define QUARK_LIKELY(x) (__builtin_expect(!!(x), 1))
#define QUARK_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#define QUARK_ALWAYS_INLINE inline __attribute__((always_inline))
#define QUARK_NOINLINE __attribute__((noinline))
#else
#define QUARK_LIKELY(x) (x)
#define QUARK_UNLIKELY(x) (x)
#define QUARK_ALWAYS_INLINE inline
#define QUARK_NOINLINE
#endif

// Cache-line alignment attribute for hot atomics / per-shard single-writer state (002/003).
#define QUARK_CACHE_ALIGNED alignas(::quark::cache_line_size)
