// Tests 020-Security §"Randomness — a PAL concern" + 019 — the ONE canonical CSPRNG entry point
// (pal::fill_random): on Linux it draws from getrandom(2); a simulation override (014) makes it a
// DETERMINISTIC, reproducible stream so a test/sim run is repeatable (the override is explicitly NOT
// secure). Proves: (1) the real path fills all requested bytes and is not trivially constant; (2) the
// sim override is deterministic for a given seed and differs across seeds; (3) install/clear works.
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "pal/csprng.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
bool all_zero(std::span<const std::byte> b) {
    for (std::byte x : b)
        if (x != std::byte{0}) return false;
    return true;
}
}  // namespace

int main() {
    bool ok = true;

    // (1) Real OS path: fills every byte, and is (astronomically likely) not all-zero.
    {
        std::array<std::byte, 64> buf{};
        const bool got = pal::fill_random(std::span<std::byte>(buf));
        check(got, "pal::fill_random succeeds (getrandom on Linux)", ok);
        check(!all_zero(std::span<const std::byte>(buf)), "random bytes are not all-zero", ok);
    }

    // (2) Deterministic sim override: same seed ⇒ identical stream; different seed ⇒ different stream.
    {
        pal::SimCsprngState s1{0xABCDEF};
        pal::set_csprng_override(pal::sim_csprng(s1));
        std::array<std::byte, 32> a{};
        (void)pal::fill_random(std::span<std::byte>(a));

        pal::SimCsprngState s2{0xABCDEF};  // same seed
        pal::set_csprng_override(pal::sim_csprng(s2));
        std::array<std::byte, 32> b{};
        (void)pal::fill_random(std::span<std::byte>(b));
        check(std::memcmp(a.data(), b.data(), a.size()) == 0,
              "sim override is deterministic for a given seed (reproducible)", ok);

        pal::SimCsprngState s3{0x123456};  // different seed
        pal::set_csprng_override(pal::sim_csprng(s3));
        std::array<std::byte, 32> c{};
        (void)pal::fill_random(std::span<std::byte>(c));
        check(std::memcmp(a.data(), c.data(), a.size()) != 0, "a different seed yields a different stream", ok);
        check(!all_zero(std::span<const std::byte>(a)), "sim stream is not all-zero", ok);
    }

    // (3) Clearing the override returns to the OS path.
    {
        pal::set_csprng_override(pal::CsprngOverride{});
        std::array<std::byte, 16> buf{};
        check(pal::fill_random(std::span<std::byte>(buf)), "cleared override ⇒ OS path again", ok);
    }

    std::printf("security_csprng_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
