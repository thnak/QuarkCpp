// Tests 020-Security §4 — the `Secret` type's two mechanical guards:
//   (1) it ZEROIZES its buffer on destruction (a key does not linger in freed heap / a core dump), and
//   (2) it is NON-COPYABLE and has NO conversion to std::string (secret bytes cannot be handed to a
//       logger/formatter by accident).
//
// The zeroization is observed WITHOUT UB by replacing global operator new/delete: at the moment the
// Secret's buffer reaches `operator delete`, the memory is still validly allocated, so we scan it there
// and assert it is all-zero (proving the destructor wiped it BEFORE the free). This replaces global
// operator new, so the CMake content rule auto-excludes it from the TSan build (expected).
//
// CONTROL (adversarial): a plain std::vector<std::byte> of the SAME distinctive size is freed WITHOUT
// wiping — the probe sees its non-zero bytes at delete time. That the Secret's block is zero and the
// plain vector's is not proves the wipe is Secret's doing, not an allocator artifact.
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

#include "quark/core/secret.hpp"

using namespace quark;

namespace {
// Distinctive watched allocation size — unlikely to collide with unrelated allocations.
constexpr std::size_t kWatchSize = 4099;

std::atomic<bool> g_armed{false};
std::atomic<void*> g_watch{nullptr};
std::atomic<int> g_result{-1};  // -1 = not observed, 1 = was all-zero at free, 0 = had nonzero bytes
}  // namespace

void* operator new(std::size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    if (n == kWatchSize && g_armed.load(std::memory_order_relaxed) &&
        g_watch.load(std::memory_order_relaxed) == nullptr) {
        g_watch.store(p, std::memory_order_relaxed);
    }
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
    if (p != nullptr && p == g_watch.load(std::memory_order_relaxed)) {
        // The block is still allocated here — scanning it is well-defined. All-zero ⇒ wiped.
        const auto* b = static_cast<const unsigned char*>(p);
        int any_nonzero = 0;
        for (std::size_t i = 0; i < kWatchSize; ++i)
            if (b[i] != 0) { any_nonzero = 1; break; }
        g_result.store(any_nonzero ? 0 : 1, std::memory_order_relaxed);
        g_watch.store(nullptr, std::memory_order_relaxed);
    }
    std::free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// --- Compile-time type guards (020 §4): non-copyable, no std::string surface. ----------------------
static_assert(!std::is_copy_constructible_v<Secret>, "Secret must be non-copyable");
static_assert(!std::is_copy_assignable_v<Secret>, "Secret must be non-copy-assignable");
static_assert(!std::is_constructible_v<std::string, Secret>,
              "Secret must NOT be constructible into a std::string");
static_assert(!std::is_convertible_v<Secret, std::string>,
              "Secret must NOT be convertible to a std::string");
static_assert(std::is_move_constructible_v<Secret>, "Secret is move-only (movable)");
}  // namespace

int main() {
    bool ok = true;

    // --- (1) Zeroization on destruction. ------------------------------------------------------------
    // Arm BEFORE the allocation: `Secret(std::move(bytes))` MOVES the vector, reusing the buffer that
    // `bytes` allocated — so the watched allocation must be the vector's, captured while armed here.
    {
        g_result.store(-1, std::memory_order_relaxed);
        g_watch.store(nullptr, std::memory_order_relaxed);
        g_armed.store(true, std::memory_order_relaxed);
        std::vector<std::byte> bytes(kWatchSize, std::byte{0xAB});  // captured: this buffer backs the Secret
        g_armed.store(false, std::memory_order_relaxed);
        {
            Secret s(std::move(bytes));  // moves the captured buffer into the Secret (no new alloc)
            check(s.size() == kWatchSize, "secret holds the material", ok);
            check(g_watch.load() != nullptr, "watched the secret's heap buffer", ok);
            // Verify the material is actually present (non-zero) before destruction.
            const auto* view = reinterpret_cast<const unsigned char*>(s.bytes().data());
            check(view[0] == 0xAB && view[kWatchSize - 1] == 0xAB, "material present pre-destruction", ok);
        }  // <- Secret destructor: secure_zero THEN free ⇒ operator delete sees an all-zero block.
        check(g_result.load() == 1, "Secret ZEROIZED its buffer on destruction (all-zero at free)", ok);
    }

    // --- CONTROL: a plain vector of the same size, freed WITHOUT a wipe, shows non-zero at free. -----
    {
        g_result.store(-1, std::memory_order_relaxed);
        g_watch.store(nullptr, std::memory_order_relaxed);
        g_armed.store(true, std::memory_order_relaxed);
        {
            std::vector<std::byte> plain(kWatchSize, std::byte{0xCD});
            g_armed.store(false, std::memory_order_relaxed);
            check(g_watch.load() != nullptr, "watched the plain vector's buffer", ok);
            // touch so the fill is not optimized away
            check(static_cast<unsigned char>(plain[0]) == 0xCD, "plain material present", ok);
        }  // <- plain vector free: no wipe.
        check(g_result.load() == 0, "CONTROL: an unwiped buffer shows NON-ZERO bytes at free", ok);
    }

    // --- Move clears the source (no lingering second copy of the material). -------------------------
    {
        Secret a = make_secret("hunter2");
        Secret b = std::move(a);
        check(a.empty() && b.size() == 7, "move transfers the material and empties the source", ok);
    }

    std::printf("security_secret_zeroize_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
