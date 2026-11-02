// #64 step 2: the instrumented allocator and the live/freed heap scan used by pinned_key_copies_check.
//
// These live in their own translation unit on purpose: with the replacement operator delete defined in the same TU as
// the code that allocates, GCC inlines free() into a caller's new-expression and reports
// -Wmismatched-new-delete, a false positive for a standard allocator replacement. Out of line, the caller only sees a
// call, and the diagnostic has no expression to latch onto.
//
// The scan is compiled OUT under ASan/TSan, where the sanitizer owns operator new/delete; enabled() then reports false
// and pinned_key_copies_check still runs its functional and lock-refusal checks.
#pragma once

#include <cstddef>
#include <span>

namespace quark::test::heap_probe {

// True when the instrumented allocator is compiled into this binary.
[[nodiscard]] bool enabled() noexcept;

// Occurrences of `needle` in every live ordinary-heap block / in the free-time quarantine.
[[nodiscard]] std::size_t scan_live(std::span<const std::byte> needle) noexcept;
[[nodiscard]] std::size_t scan_freed(std::span<const std::byte> needle) noexcept;

// False when the allocation table or the quarantine overflowed: the scan would then be incomplete, so a clean result
// must not be trusted.
[[nodiscard]] bool complete() noexcept;

}  // namespace quark::test::heap_probe
