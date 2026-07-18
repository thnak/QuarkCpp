// Skeleton smoke test — proves the scaffolding compiles, links, and the core vocabulary
// (config/ids/error/pal) is coherent. Replaced/augmented by real invariant tests as modules land.
#include <cassert>
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

    // ids — placement hash is deterministic and content-addressed (026)
    const quark::ActorId a{quark::TypeKey{0xABCD}, 42};
    const quark::ActorId b{quark::TypeKey{0xABCD}, 42};
    assert(a == b);
    assert(a.hash() == b.hash());
    assert((quark::ActorId{quark::TypeKey{0xABCD}, 43}).hash() != a.hash());

    // error — result<T> round-trips value and error
    const quark::result<int> ok{7};
    assert(ok && *ok == 7);
    const quark::result<int> err = quark::fail(quark::errc::not_found, "actor");
    assert(!err && err.error().code == quark::errc::not_found);

    // pal — barrier is callable, clock is monotonic
    quark::pal::store_load_barrier();
    const auto t0 = quark::pal::now();
    const auto t1 = quark::pal::now();
    assert(t1 >= t0);

    std::printf("quark %s: smoke OK (cache_line=%zu, abi=0x%08X)\n", quark::version_string(),
                quark::cache_line_size, quark::pal::platform_abi_tag);
    return 0;
}
