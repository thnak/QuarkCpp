// #64 step 2: the heap probe behind pinned_key_copies_check — an operator new/delete replacement that records every
// ordinary-heap allocation and snapshots each block into a bounded quarantine at free time, so a key copy that is
// freed before the scan is still visible. Single-threaded by construction (the test starts no threads).
//
// Deliberately its own translation unit (see heap_probe.hpp): GCC's -Wmismatched-new-delete is a false positive for a
// standard allocator replacement when the replacement is inlined into the caller, and splitting the TU removes the
// inlining rather than suppressing the warning.
#include "heap_probe.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_MSC_VER)
#  include <malloc.h>
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define QUARK_HEAP_PROBE 0
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define QUARK_HEAP_PROBE 0
#  else
#    define QUARK_HEAP_PROBE 1
#  endif
#else
#  define QUARK_HEAP_PROBE 1
#endif

#if QUARK_HEAP_PROBE

namespace {

struct Slot {
    void* p;
    std::size_t n;
};
void* const kTombstone = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
constexpr std::size_t kSlotCount = 1u << 17;
Slot g_slots[kSlotCount];  // zero-initialised statically: recording never allocates
bool g_table_overflow = false;

constexpr std::size_t kFreedMax = 1u << 16;
constexpr std::size_t kQuarantineCap = 32u << 20;
struct Freed {
    std::size_t off;
    std::size_t len;
};
Freed g_freed[kFreedMax];
std::size_t g_freed_n = 0;
std::byte g_quarantine[kQuarantineCap];
std::size_t g_quarantine_used = 0;
bool g_quarantine_overflow = false;

[[nodiscard]] std::size_t slot_of(void* p) noexcept {
    auto x = reinterpret_cast<std::uintptr_t>(p);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return static_cast<std::size_t>(x) & (kSlotCount - 1);
}

void record(void* p, std::size_t n) noexcept {
    const std::size_t start = slot_of(p);
    std::size_t first_tomb = kSlotCount;
    for (std::size_t k = 0; k < kSlotCount; ++k) {
        Slot& s = g_slots[(start + k) & (kSlotCount - 1)];
        if (s.p == p) {
            s.n = n;
            return;
        }
        if (s.p == kTombstone && first_tomb == kSlotCount) first_tomb = (start + k) & (kSlotCount - 1);
        if (s.p == nullptr) {
            Slot& put = first_tomb == kSlotCount ? s : g_slots[first_tomb];
            put.p = p;
            put.n = n;
            return;
        }
    }
    g_table_overflow = true;
}

// Removes `p` and returns its size (0 when unknown).
[[nodiscard]] std::size_t take(void* p) noexcept {
    const std::size_t start = slot_of(p);
    for (std::size_t k = 0; k < kSlotCount; ++k) {
        Slot& s = g_slots[(start + k) & (kSlotCount - 1)];
        if (s.p == nullptr) return 0;
        if (s.p == p) {
            const std::size_t n = s.n;
            s.p = kTombstone;
            s.n = 0;
            return n;
        }
    }
    return 0;
}

void quarantine(void* p) noexcept {
    const std::size_t n = take(p);
    if (n == 0) return;
    if (g_freed_n == kFreedMax || n > kQuarantineCap - g_quarantine_used) {
        g_quarantine_overflow = true;
        return;
    }
    std::memcpy(g_quarantine + g_quarantine_used, p, n);
    g_freed[g_freed_n].off = g_quarantine_used;
    g_freed[g_freed_n].len = n;
    ++g_freed_n;
    g_quarantine_used += n;
}

[[nodiscard]] std::size_t count_in(std::span<const std::byte> hay, std::span<const std::byte> needle) noexcept {
    if (needle.empty() || hay.size() < needle.size()) return 0;
    std::size_t hits = 0;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (std::memcmp(hay.data() + i, needle.data(), needle.size()) == 0) ++hits;
    return hits;
}

}  // namespace

#endif  // QUARK_HEAP_PROBE

namespace quark::test::heap_probe {

bool enabled() noexcept { return QUARK_HEAP_PROBE != 0; }

#if QUARK_HEAP_PROBE
std::size_t scan_live(std::span<const std::byte> needle) noexcept {
    std::size_t hits = 0;
    for (const Slot& s : g_slots)
        if (s.p != nullptr && s.p != kTombstone)
            hits += count_in(std::span<const std::byte>(static_cast<const std::byte*>(s.p), s.n), needle);
    return hits;
}

std::size_t scan_freed(std::span<const std::byte> needle) noexcept {
    std::size_t hits = 0;
    for (std::size_t i = 0; i < g_freed_n; ++i)
        hits += count_in(std::span<const std::byte>(g_quarantine + g_freed[i].off, g_freed[i].len), needle);
    return hits;
}

bool complete() noexcept { return !g_table_overflow && !g_quarantine_overflow; }
#else
std::size_t scan_live(std::span<const std::byte>) noexcept { return 0; }
std::size_t scan_freed(std::span<const std::byte>) noexcept { return 0; }
bool complete() noexcept { return true; }
#endif

}  // namespace quark::test::heap_probe

#if QUARK_HEAP_PROBE

// ---- global operator new/delete replacement (only when the probe is compiled in) -------------------
// The nothrow forms are NOT replaced: the standard's default ones delegate to these throwing forms.
void* operator new(std::size_t n) {
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    record(p, n == 0 ? 1 : n);
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {
    if (p == nullptr) return;
    quarantine(p);
    std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

void* operator new(std::size_t n, std::align_val_t a) {
    const std::size_t al = static_cast<std::size_t>(a);
    void* p = nullptr;
#  if defined(_MSC_VER)
    p = _aligned_malloc(n == 0 ? 1 : n, al);
#  else
    const std::size_t sz = ((n == 0 ? 1 : n) + al - 1) / al * al;
    if (posix_memalign(&p, al, sz) != 0) p = nullptr;
#  endif
    if (p == nullptr) throw std::bad_alloc();
    record(p, n == 0 ? 1 : n);
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept {
    if (p == nullptr) return;
    quarantine(p);
#  if defined(_MSC_VER)
    _aligned_free(p);
#  else
    std::free(p);
#  endif
}
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }

#endif  // QUARK_HEAP_PROBE
