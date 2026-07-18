// Implements the 013/ADR-008 live-reconfig + validation + Frozen-Core invariants:
//   * LIVE: `Engine::reconfigure(delta)` changes an operational field observably at the NEXT read.
//   * FROZEN-CORE (BuildOnly): structural knobs (worker/shard count, …) cannot be changed live —
//     enforced AT COMPILE TIME (they are not `OperationalDelta` members and the engine exposes no
//     live setter), which is stronger than the ADR-008 runtime `ReconfigError::BuildOnly` fail-fast.
//   * VALIDATION: an out-of-range value fails fast with `errc::validation` (never truncates), both on
//     `reconfigure()` and on `ConfigBuilder::build()`.
//
// Pin: taskset -c 0-3 (single-threaded — no workers started). Run under ASan/UBSan and TSan.
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "quark/core/engine.hpp"

using namespace quark;

// NDEBUG-independent check (the sanitizer builds are Release, which would strip assert()).
namespace {
bool g_ok = true;
}
#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "CHECK FAILED (%s:%d): %s\n", __FILE__, __LINE__, (msg)); \
            g_ok = false;                                                           \
        }                                                                           \
    } while (0)

// ---- FROZEN-CORE = compile-time BuildOnly (the strongest form of the ADR-008 gate) ----
// A Live delta may carry ONLY operational (Hot-Leaf) knobs. Frozen-Core fields are simply not
// expressible, so a live change to them cannot even be written. Named concepts keep the member probe
// a *dependent* expression, so a missing member is a SOFT (SFINAE) failure on both g++ and clang++.
template <class D> concept has_drain_field  = requires(D d) { d.drain_budget; };
template <class D> concept has_bound_field  = requires(D d) { d.mailbox_bound; };
template <class D> concept has_idle_field   = requires(D d) { d.idle_ticks; };
template <class D> concept has_worker_field = requires(D d) { d.worker_count; };
template <class D> concept has_shard_field  = requires(D d) { d.shard_count; };
template <class D> concept has_band_field   = requires(D d) { d.band_count; };
template <class E> concept has_set_worker   = requires(E& e) { e.set_worker_count(2u); };
template <class E> concept has_set_shard    = requires(E& e) { e.set_shard_count(2u); };

static_assert(has_drain_field<OperationalDelta>, "drain_budget IS a Live (Hot-Leaf) knob");
static_assert(has_bound_field<OperationalDelta>, "mailbox_bound IS a Live knob");
static_assert(has_idle_field<OperationalDelta>, "idle_ticks IS a Live knob");
static_assert(!has_worker_field<OperationalDelta>, "worker_count is FROZEN-CORE — not a Live field");
static_assert(!has_shard_field<OperationalDelta>, "shard_count is FROZEN-CORE — not a Live field");
static_assert(!has_band_field<OperationalDelta>, "band_count is FROZEN-CORE — not a Live field");
// The engine offers no live setter for a frozen field.
static_assert(!has_set_worker<Engine<>>, "Engine must expose no live worker_count setter (BuildOnly)");
static_assert(!has_set_shard<Engine<>>, "Engine must expose no live shard_count setter (BuildOnly)");

int main() {
    // --- Build a validated config via the builder (013 programmatic source of truth). ---
    const auto built = ConfigBuilder{}
                           .workers(1)
                           .shards(1)
                           .default_drain_budget(64)
                           .default_mailbox_bound(4096, Overflow::Block)
                           .validation(Validation::Strict)
                           .build();
    CHECK(built.has_value(), "valid config must build");
    if (!built) return 1;

    Engine<> eng(*built);

    // --- LIVE: reconfigure changes an operational field observably at the next read. ---
    CHECK(eng.default_drain_budget() == 64, "seeded default drain budget visible");
    {
        const auto r = eng.reconfigure(OperationalDelta{.drain_budget = 512});
        CHECK(r.has_value(), "a valid Live delta must succeed");
        CHECK(eng.default_drain_budget() == 512, "new budget visible at the next read");
        CHECK(r && unpack_operational(r->published_word).drain_budget == 512, "receipt word carries the new budget");
        CHECK(r && r->masked_count == 0, "engine-wide cell masks no per-instance overrides");
    }
    // Other Live fields are equally observable, and unspecified fields are preserved.
    {
        const auto r = eng.reconfigure(OperationalDelta{.mailbox_bound = 16384, .overflow = Overflow::DropOldest});
        CHECK(r.has_value(), "multi-field Live delta must succeed");
        const OperationalConfig now = eng.operational().decode();
        CHECK(now.mailbox_bound == 16384, "mailbox_bound observable");
        CHECK(now.overflow == Overflow::DropOldest, "overflow observable");
        CHECK(now.drain_budget == 512, "unspecified fields preserved across reconfig");
    }

    // --- VALIDATION: out-of-range Live values fail fast with errc::validation (no truncation). ---
    {
        const auto r = eng.reconfigure(OperationalDelta{.drain_budget = 0});
        CHECK(!r && r.error().code == errc::validation, "drain_budget=0 → errc::validation");
        CHECK(eng.default_drain_budget() == 512, "a rejected reconfig mutates NOTHING");
    }
    {
        const auto r = eng.reconfigure(OperationalDelta{.drain_budget = 16384});  // == 2^14 ceiling
        CHECK(!r && r.error().code == errc::validation, "drain_budget at ceiling → errc::validation");
    }
    {
        const auto r = eng.reconfigure(OperationalDelta{.mailbox_bound = 0x100'0000});  // == 2^24 ceiling
        CHECK(!r && r.error().code == errc::validation, "mailbox_bound at ceiling → errc::validation");
    }
    {
        const auto r = eng.reconfigure(OperationalDelta{.log_level = 8});  // >= 8
        CHECK(!r && r.error().code == errc::validation, "log_level >= 8 → errc::validation");
    }

    // --- VALIDATION at build() (008 Strict fail-fast at load). ---
    CHECK(!ConfigBuilder{}.shards(0).build(), "shards=0 must fail validation");
    CHECK(ConfigBuilder{}.shards(0).build().error().code == errc::validation, "shards=0 → errc::validation");
    CHECK(!ConfigBuilder{}.default_drain_budget(0).build(), "budget=0 must fail validation");
    CHECK(!ConfigBuilder{}.bands(9).build(), "band_count>8 must fail validation");
    CHECK(!ConfigBuilder{}.default_drain_budget(100000).build(), "budget over ceiling must fail");

    if (!g_ok) return 1;
    std::printf("PASS: live reconfig observable; frozen fields BuildOnly (compile-time); "
                "out-of-range -> errc::validation\n");
    return 0;
}
