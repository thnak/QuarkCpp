// Skeleton smoke test — proves the scaffolding compiles, links, and the core vocabulary
// (config/ids/error/pal) is coherent. Replaced/augmented by real invariant tests as modules land.
// Checks are plain `if`s rather than assert(): the default build is Release (NDEBUG), which
// compiles assert() out entirely — an assert-only smoke test would trivially pass unchecked.
#include <cstdio>

#include "quark/core/config.hpp"
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/version.hpp"
#include "pal/pal.hpp"

int main() {
    // config
    static_assert(quark::max_descriptor_size == 64, "descriptor ceiling is one cache line (003)");
    static_assert(quark::cache_line_size >= 64);

    bool ok = true;

    // ids — placement hash is deterministic and content-addressed (026)
    const quark::ActorId a{quark::TypeKey{0xABCD}, 42};
    const quark::ActorId b{quark::TypeKey{0xABCD}, 42};
    if (!(a == b)) ok = false;
    if (!(a.hash() == b.hash())) ok = false;
    if (!((quark::ActorId{quark::TypeKey{0xABCD}, 43}).hash() != a.hash())) ok = false;

    // error — result<T> round-trips value and error
    const quark::result<int> good{7};
    if (!(good && *good == 7)) ok = false;
    const quark::result<int> err = quark::fail(quark::errc::not_found, "actor");
    if (!(!err && err.error().code == quark::errc::not_found)) ok = false;

    // pal — barrier is callable, clock is monotonic
    quark::pal::store_load_barrier();
    const auto t0 = quark::pal::now();
    const auto t1 = quark::pal::now();
    if (!(t1 >= t0)) ok = false;

    std::printf("quark %s: smoke %s (cache_line=%zu, abi=0x%08X)\n", quark::version_string(),
                ok ? "OK" : "FAIL", quark::cache_line_size, quark::pal::platform_abi_tag);
    return ok ? 0 : 1;
}
