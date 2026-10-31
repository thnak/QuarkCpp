// Implements 019-Platform-Abstraction-Layer §"Android target" / ADR-071 §3 (toolchain gaps): a relaxed
// atomic LOAD of a plain (non-atomic) object another thread writes — `std::atomic_ref<T>(x).load(relaxed)`.
// NDK r28 libc++ (LLVM 19) has no `std::atomic_ref`; the GNU/Clang builtin is the same single instruction.
// Used only by the ADR-022 histogram scraper snapshot, whose writer side is deliberately plain.
#pragma once

#include <atomic>
#include <cstdint>
#include <version>

namespace quark::detail {

template <class T>
[[nodiscard]] inline T load_relaxed(T& x) noexcept {
#if defined(__cpp_lib_atomic_ref) && __cpp_lib_atomic_ref >= 201806L
    return std::atomic_ref<T>(x).load(std::memory_order_relaxed);
#elif defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&x, __ATOMIC_RELAXED);
#else
#error "quark::detail::load_relaxed needs std::atomic_ref or a GNU-style __atomic_load_n"
#endif
}

}  // namespace quark::detail
