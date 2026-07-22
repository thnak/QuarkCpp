// Implements 008-Metadata-and-Startup — the pipeline that turns registered actor TYPES into a
// stable content-addressed identity (`TypeKey`), a compile-time-gathered `ActorMetadata` descriptor,
// startup Validation (fail-fast), and a `TypeKey → {metadata, factory}` registry. No RTTI, no
// reflection, no virtual: everything is derived from the CRTP policy pack (005 policies.hpp), the
// dispatch Protocol (ADR-007 dispatch.hpp), the supervision pack (007 supervision.hpp), the resource
// model (004 resource.hpp), and the reflection-free fingerprint (016 describe.hpp), then flattened.
//
// TYPE IDENTITY (008 §Type identity). `type_key_of<T>()` is the DURABLE, cross-run/cross-node key:
//   * a Described message type → its 016 `fingerprint_v<T>` (toolchain-independent; the SAME key the
//     durable record header (016) and wire negotiation (010) already use — this is the promised
//     unification, replacing the ad-hoc process-local sentinel address 006/016 carried);
//   * an actor type → a fold of a canonical-type-name FNV-1a hash with each protocol message's key
//     (so two actors with the same protocol but different names stay distinct, and the described-
//     message part of the fold is toolchain-independent);
//   * any other type → the canonical-type-name FNV-1a hash (008's stated derivation).
// The name normalization across GCC/Clang/MSVC is the standing 008 open question (a mixed-toolchain
// cluster needs a conformance test or an explicit per-type key); within one toolchain it is stable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/activation.hpp"   // ReconstructSink, SupervisionPolicy, DispatchTable, Activation
#include "quark/core/describe.hpp"     // Described, fingerprint_v (016)
#include "quark/core/dispatch.hpp"     // Protocol
#include "quark/core/engine_config.hpp"  // Validation mode (Strict/Permissive)
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"  // Store, PersistMode, Persistent<> (ADR-028 Phase 5 recover seam)
#include "quark/core/policies.hpp"     // is_actor, priority_band_of, drain_budget_of, max_concurrency_of, ...
#include "quark/core/resource.hpp"     // ResourceScope (004 cold wire pass)
#include "quark/core/snapshot.hpp"     // recover_snapshot<State> (ADR-028 Phase 5 recover seam)
#include "quark/core/supervision.hpp"  // supervision_of<A>()
#include "quark/detail/hash.hpp"       // hash_combine (folds the protocol keys)

namespace quark {

// ============================================================================================
// 008 §Type identity — the content-addressed TypeKey derivation (RTTI-free, constexpr).
// ============================================================================================
namespace detail {

// FNV-1a over raw bytes (008 §Type identity: "constexpr FNV-1a hash of a canonical type name").
[[nodiscard]] constexpr std::uint64_t fnv1a(std::string_view s) noexcept {
    std::uint64_t h = 1469598103934665603ULL;  // FNV offset basis (matches FingerprintFolder)
    for (char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(c));
        h *= 1099511628211ULL;  // FNV prime
    }
    return h;
}

// The compiler's function-name string, sliced to just the `T = …` payload — a canonical type name
// without a reflection library (008 §Dependencies). Stable within one toolchain; cross-toolchain
// normalization is the documented open question. Only the BYTES are folded (never a pointer stored),
// so this is safe in a constant expression.
template <class T>
[[nodiscard]] constexpr std::string_view canonical_type_name() noexcept {
#if defined(__clang__)
    constexpr std::string_view fn = __PRETTY_FUNCTION__;
    constexpr std::string_view lead = "[T = ";
#elif defined(__GNUC__)
    constexpr std::string_view fn = __PRETTY_FUNCTION__;
    constexpr std::string_view lead = "[with T = ";
#else
    constexpr std::string_view fn = __func__;
    constexpr std::string_view lead = "";
#endif
    const std::size_t at = fn.find(lead);
    if (at == std::string_view::npos) return fn;
    const std::size_t start = at + lead.size();
    // GCC appends `; std::string_view = …]`; Clang closes with `]`. Stop at whichever comes first.
    std::size_t end = fn.find_first_of(";]", start);
    if (end == std::string_view::npos) end = fn.size();
    return fn.substr(start, end - start);
}

// The key of a single (non-actor) type: a Described type folds to its 016 fingerprint (toolchain-
// independent, and identical to the durable/wire key); anything else to its canonical-name hash.
template <class T>
[[nodiscard]] constexpr std::uint64_t single_type_key() noexcept {
    if constexpr (Described<T>) {
        return fingerprint_v<T>;
    } else {
        return fnv1a(canonical_type_name<T>());
    }
}

// Fold an actor's protocol message set into its type key (008 §Type identity: "for actors, fold the
// protocol/handler set"). Seeded with the actor's own canonical-name hash so same-protocol actors
// stay distinct; each message key is order-sensitively combined so a reordered protocol differs.
template <class A, class... Ms>
[[nodiscard]] constexpr std::uint64_t fold_protocol(Protocol<Ms...>*) noexcept {
    std::uint64_t h = fnv1a(canonical_type_name<A>());
    ((h = hash_combine(h, single_type_key<Ms>())), ...);
    return h;
}

template <class T>
[[nodiscard]] constexpr std::uint64_t raw_type_key() noexcept {
    if constexpr (is_actor<T> && requires { typename T::protocol; }) {
        return fold_protocol<T>(static_cast<typename T::protocol*>(nullptr));
    } else if constexpr (Described<T>) {
        return fingerprint_v<T>;  // matches the 016 durable/wire fingerprint exactly
    } else {
        return fnv1a(canonical_type_name<T>());
    }
}

}  // namespace detail

// The stable, content-addressed TypeKey for `T` (008). Deterministic across runs and (for the
// Described / protocol-fingerprint part) across nodes. Constexpr, so it is usable both at compile
// time (durable headers, dispatch identity) and at runtime (the registry).
template <class T>
[[nodiscard]] constexpr TypeKey type_key_of() noexcept {
    return TypeKey{detail::raw_type_key<T>()};
}

// Actor identity `(TypeKey, instance key)` (006 keying, moved here from actor_ref.hpp so the durable
// TypeKey is the single source of truth — the process-local sentinel address is retired).
template <class A>
[[nodiscard]] constexpr ActorId actor_id_of(std::uint64_t key) noexcept {
    return ActorId{type_key_of<A>(), key};
}

// ============================================================================================
// 008 §Metadata compilation — the compile-time gather of an actor's descriptor.
// ============================================================================================

// The type-erased construction factory (008 §Metadata compilation `ConstructFn`). Heap-allocates a
// fresh actor; `destroy` frees it. Both are plain `.rodata` function pointers (no virtual).
using ConstructFn = void* (*)();
using DestroyFn = void (*)(void*) noexcept;

// ADR-028 Phase 4: the type-erased resource wire pass — `A::wire(scope)` through a `void* self`, so
// the broker's lazy first-construction can wire `Cached<>`/`PerMessage<>` members exactly like
// `spawn<A>()` does today (004 §Rules). Null iff `A` declares no `wire()` (has_resource_wire<A> below
// is false) — a pure function of `A`, compiled unconditionally by `compile_actor_metadata<A>()`.
// `WireFn` itself now lives in `resource.hpp` ("Phase 6" redirected — `Activation` needs the type too
// to re-wire after a 007 Restart, and can't depend on this header without a cycle).

template <class A>
[[nodiscard]] WireFn make_wire_fn() noexcept;  // defined after has_resource_wire<A> below

// ADR-028 Phase 5: the type-erased persistence-recovery thunk — erases BOTH `A` (via `void* self`)
// AND the concrete `Store` type `S` (via `void* store`), closing over both as template parameters at
// the one call site that knows them concretely (`TypeRegistry::register_type<A,S>`). Null iff `A`
// declares no `Persistent<Snapshot,...>` policy (`is_snapshot_persistent_v<A>` below is false). Called
// at most once per `ActorId`, from the broker's one-time construction (ADR-028 Phase 4 `handle_wake`)
// — a cold path, same cost profile as `WireFn`, no virtual dispatch anywhere.
using RecoverFn = result<void> (*)(void* self, void* store, ActorId id);

template <class A, class S>
[[nodiscard]] RecoverFn make_recover_fn() noexcept;  // defined after has_persist_state<A> below

// The RECONSTRUCT factory (007/ADR-009 §Restart): destroy the actor's state in place and
// placement-new a FRESH instance at the same address, so `Restart` produces genuinely fresh state
// (not assert-intact). Wired into `Activation::set_reconstruct`. Requires `A` default-constructible.
template <class A>
[[nodiscard]] ReconstructSink make_reconstruct_sink() noexcept {
    static_assert(std::is_default_constructible_v<A>,
                  "make_reconstruct_sink<A>: Restart reconstruction needs A default-constructible");
    return ReconstructSink{
        +[](void* self, void*) noexcept {
            A* a = static_cast<A*>(self);
            a->~A();
            ::new (a) A();  // fresh state at the same storage; Activation.self_ stays valid
        },
        nullptr};
}

template <class A>
[[nodiscard]] ConstructFn make_construct_fn() noexcept {
    static_assert(std::is_default_constructible_v<A>, "make_construct_fn<A>: A must be default-constructible");
    return +[]() -> void* { return new A(); };
}
template <class A>
[[nodiscard]] DestroyFn make_destroy_fn() noexcept {
    return +[](void* p) noexcept { delete static_cast<A*>(p); };
}

// The materialized per-actor metadata record (008 §Metadata compilation). A focused, flat descriptor
// gathered ONCE at registration from the CRTP policy pack + dispatch table + supervision + factories.
// Records live in a flat array indexed by `index`; a parallel `TypeKey → index` map handles the
// wire/storage lookups (008 §Metadata compilation).
struct ActorMetadata {
    TypeKey key{};                       // durable content-addressed identity (008)
    std::uint16_t index = 0;             // dense counter assigned at registration (hot-path array idx)
    DispatchTable dispatch{};            // ADR-007 dense-slot .rodata thunk array
    std::size_t max_concurrency = 1;     // 005/015 (1 = Sequential; 0 = Reentrant; N = MaxConcurrency)
    std::uint16_t band = 0;              // 005/ADR-010 priority band (0 = highest)
    std::uint32_t drain_budget = 0;      // 005 per-actor budget; 0 ⇒ use the engine-wide default (013)
    SupervisionPolicy supervision{};     // 007 resolved OnFailure<…>
    bool keep_alive = false;             // 005/013 lifecycle
    std::uint64_t idle_timeout_ms = 0;   // 005/013 lifecycle (0 ⇒ none / KeepAlive)
    ConstructFn construct = nullptr;     // 008 factory: build a fresh actor
    DestroyFn destroy = nullptr;         // 008 factory: free it
    ReconstructSink reconstruct{};       // 007 factory: fresh-state Restart
    // ADR-028 Phase 4 — the lazy-activation broker's construct_and_wire seam. `wire` is a pure
    // function of `A` (null iff `A` declares no `wire()`), compiled unconditionally below; `scope`/
    // `reclaim` are set at REGISTRATION (not compile) time by `TypeRegistry::register_type` — the
    // same `ResourceScope`/`ReclaimSink` a `spawn<A>()` caller would pass, stored so the broker's
    // first real construction can wire resources exactly once (004 §Rules; ADR-021 no re-resolution).
    WireFn wire = nullptr;
    const ResourceScope* scope = nullptr;
    ReclaimSink reclaim{};
    // ADR-028 Phase 5 — the lazy-activation broker's one-time persistence-recovery seam. `recover` is
    // a pure function of `<A, S>` (S = the concrete Store type; null iff `A` declares no
    // `Persistent<Snapshot,...>` policy), compiled by `TypeRegistry::register_type<A,S>` (NOT by
    // `compile_actor_metadata<A>()` — exactly like `wire` above, an eagerly-`spawn`'d Persistent<Snapshot>
    // actor is untouched unless it goes through the new store-taking registration overload). `store` is
    // the type-erased pointer to the caller's concrete Store instance, set at the same registration call.
    RecoverFn recover = nullptr;
    void* store = nullptr;
};

// Compile-time gather (008 §Metadata compilation). All fields are pure functions of `A`'s policy
// pack / protocol / supervision; the factories are `.rodata` thunks. The `static_assert`s are the
// compile-time half of Validation (008 §Validation "Compile time") — malformed policy lists never
// reach runtime.
template <class A>
[[nodiscard]] ActorMetadata compile_actor_metadata() noexcept {
    static_assert(is_actor<A>, "compile_actor_metadata<A>: A must derive from quark::Actor<A, ...>");
    static_assert(validate_actor_policies<A>(), "compile_actor_metadata<A>: policy validation failed");
    ActorMetadata m{};
    m.key = type_key_of<A>();
    m.dispatch = A::dispatch_table();
    m.max_concurrency = max_concurrency_of<A>();
    m.band = static_cast<std::uint16_t>(priority_band_of<A>());
    m.drain_budget = drain_budget_of<A>();
    m.supervision = supervision_of<A>();
    m.keep_alive = keeps_alive_v<A>;
    m.idle_timeout_ms = idle_timeout_ms_of<A>();
    m.construct = make_construct_fn<A>();
    m.destroy = make_destroy_fn<A>();
    m.reconstruct = make_reconstruct_sink<A>();
    m.wire = make_wire_fn<A>();  // ADR-028 Phase 4: null iff A has no wire() (has_resource_wire<A>)
    return m;
}

// An actor exposes its 004 resource members for the one-time cold wire pass via a member
// `result<void> wire(const ResourceScope&)` (the explicit-list analogue of the dispatch Protocol —
// std C++23 cannot enumerate members). Actors with no resources simply omit it.
template <class A>
concept has_resource_wire = requires(A& a, const ResourceScope& s) {
    { a.wire(s) } -> std::same_as<result<void>>;
};

// ADR-028 Phase 4: the type-erased wire thunk (forward-declared above `ActorMetadata`, defined here
// now that `has_resource_wire<A>` exists). Null for an A with no `wire()` — the broker's construct
// path skips wiring entirely for such actors, exactly like `spawn<A>()`'s existing `if constexpr`.
template <class A>
[[nodiscard]] WireFn make_wire_fn() noexcept {
    if constexpr (has_resource_wire<A>) {
        return +[](void* self, const ResourceScope& scope) -> result<void> {
            return static_cast<A*>(self)->wire(scope);
        };
    } else {
        return nullptr;
    }
}

// ADR-028 Phase 5: an actor opts into lazy persistence-recovery via a member `PersistState` alias
// (must satisfy `Described` — 016 `QUARK_SERIALIZE`'d, so it can round-trip through `recover_snapshot`)
// plus `snapshot_state()` (capture current state; also the "seed" default when no snapshot exists yet)
// and `restore_state(PersistState)` (apply recovered/seeded state right after construction, before any
// message is dispatched). Mirrors `has_resource_wire<A>` exactly: opt-in, member-detected, no virtual.
template <class A>
concept has_persist_state = requires(A& a, typename A::PersistState st) {
    typename A::PersistState;
    { a.snapshot_state() } -> std::same_as<typename A::PersistState>;
    { a.restore_state(std::move(st)) } -> std::same_as<void>;
} && Described<typename A::PersistState>;

// ADR-028 Phase 5: the type-erased recover thunk (forward-declared above `ActorMetadata`, defined here
// now that `has_persist_state<A>` exists). Null unless `A` declares `Persistent<Snapshot,...>` — a
// pure function of `<A, S>`, compiled by `TypeRegistry::register_type<A,S>` (never by
// `compile_actor_metadata<A>()`, so an eagerly-`spawn`'d Persistent<Snapshot> actor with no
// `PersistState` contract, e.g. the 07_persistence sample, is entirely unaffected).
template <class A, class S>
[[nodiscard]] RecoverFn make_recover_fn() noexcept {
    if constexpr (is_snapshot_persistent_v<A>) {
        static_assert(has_persist_state<A>,
                      "declare_lazy<A>(store, ...): A declares Persistent<Snapshot,...> but is missing "
                      "the PersistState alias + snapshot_state()/restore_state() contract (ADR-028 "
                      "Phase 5)");
        static_assert(Store<S>, "declare_lazy<A>(store, ...): store must model the 012 Store concept");
        return +[](void* self, void* store, ActorId id) -> result<void> {
            A* a = static_cast<A*>(self);
            S& s = *static_cast<S*>(store);
            auto rec = recover_snapshot<typename A::PersistState>(s, id, a->snapshot_state());
            if (!rec) return std::unexpected(rec.error());
            a->restore_state(std::move(rec->state));
            return {};
        };
    } else {
        return nullptr;
    }
}

// ============================================================================================
// 008 §Validation — the startup ValidationReport (fail-fast in Strict, warn+continue in Relaxed).
// ============================================================================================
enum class Severity : std::uint8_t { Warning = 0, Error = 1 };

// One `{severity, code, subject, message}` finding (008 §Validation). `subject` is the offending
// type's key; `message` is a borrowed static string (errors never own heap, matching `quark::error`).
struct ValidationEntry {
    Severity severity = Severity::Error;
    errc code = errc::validation;
    TypeKey subject{};
    std::string_view message{};
};

struct ValidationReport {
    std::vector<ValidationEntry> entries;
    [[nodiscard]] bool has_error() const noexcept {
        for (const auto& e : entries)
            if (e.severity == Severity::Error) return true;
        return false;
    }
    [[nodiscard]] std::size_t warnings() const noexcept {
        std::size_t n = 0;
        for (const auto& e : entries)
            if (e.severity == Severity::Warning) ++n;
        return n;
    }
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
};

// ============================================================================================
// 008 §Type registry — `TypeKey → {metadata, factory}` + the dense `type_index` assignment.
// ============================================================================================
class TypeRegistry {
public:
    explicit TypeRegistry(Validation mode = Validation::Strict, std::uint32_t max_types = 256) noexcept
        : mode_(mode), max_types_(max_types) {}

    // Register actor type `A` with an optional resource-wiring validation closure (004): `check()`
    // returns `result<void>` — e.g. `[&]{ return wire_resources(scope, a.x_, a.y_); }` — so an
    // undeclared resource fails registration with `errc::validation` (008 §Validation "Startup").
    // Strict: any error returns `unexpected` and publishes nothing. Relaxed: the error is downgraded
    // to a warning and the type is registered (quarantined).
    // ADR-028 Phase 4: `scope`/`reclaim` are stored into the published record (not just validated
    // against) so a LATER real construction — the broker's first-touch activation — can wire
    // resources exactly like `spawn<A>()` does today. Both default to inert (no scope, default
    // reclaim), matching every pre-Phase-4 call site's behavior byte-for-byte.
    template <class A, class WireCheck>
    [[nodiscard]] result<std::uint16_t> register_type(WireCheck&& check,
                                                      const ResourceScope* scope = nullptr,
                                                      ReclaimSink reclaim = {}) {
        return register_metadata(compile_actor_metadata<A>(), detail::canonical_type_name<A>(),
                                 std::forward<WireCheck>(check), scope, reclaim);
    }
    template <class A>
    [[nodiscard]] result<std::uint16_t> register_type(const ResourceScope* scope = nullptr,
                                                      ReclaimSink reclaim = {}) {
        return register_type<A>([] { return result<void>{}; }, scope, reclaim);
    }

    // ADR-028 Phase 5: register `A` against a concrete `Store` `store` so the broker's one-time lazy
    // construction (Phase 4 `handle_wake`) generically recovers persisted state via `recover_snapshot`
    // (`A` must declare `Persistent<Snapshot,...>` — enforced by the caller, `Engine::declare_lazy`, via
    // `static_assert(is_snapshot_persistent_v<A>)`, so a misuse fails to compile at the call site rather
    // than silently registering an inert `recover`). `store` must outlive every activation of `A` (the
    // SAME lifetime contract `ResourceScope*`/`ReclaimSink` already carry for `scope`/`reclaim`).
    template <class A, class S>
        requires Store<S>
    [[nodiscard]] result<std::uint16_t> register_type(S& store, const ResourceScope* scope = nullptr,
                                                      ReclaimSink reclaim = {}) {
        ActorMetadata m = compile_actor_metadata<A>();
        m.recover = make_recover_fn<A, S>();
        m.store = static_cast<void*>(&store);
        return register_metadata(std::move(m), detail::canonical_type_name<A>(),
                                 [] { return result<void>{}; }, scope, reclaim);
    }

    // Low-level publish of a compiled record (008 §Metadata compilation; ADR-008 add-type). Runs the
    // startup Validation set: `type_key` collision scan vs the registered set, cap check, then the
    // caller's `check()`. `subject` is a borrowed name for diagnostics.
    template <class WireCheck>
    [[nodiscard]] result<std::uint16_t> register_metadata(ActorMetadata m, std::string_view subject,
                                                          WireCheck&& check,
                                                          const ResourceScope* scope = nullptr,
                                                          ReclaimSink reclaim = {}) {
        (void)subject;
        m.scope = scope;
        m.reclaim = reclaim;
        // --- type_key collision (008 §Type identity: distinct types hashing equal is a Strict fail).
        if (const auto it = by_key_.find(m.key); it != by_key_.end()) {
            report_.entries.push_back(
                {Severity::Error, errc::validation, m.key, "type_key collision (distinct types hash equal)"});
            if (mode_ == Validation::Strict)
                return fail(errc::validation, "type_key collision (distinct types hash equal)");
            return it->second;  // Relaxed: reuse the incumbent index, do not double-register.
        }
        // --- capacity (ADR-008: arrays pre-sized to a max_types cap).
        if (records_.size() >= max_types_) {
            report_.entries.push_back(
                {Severity::Error, errc::validation, m.key, "max_types cap exceeded"});
            if (mode_ == Validation::Strict) return fail(errc::validation, "max_types cap exceeded");
            return fail(errc::validation, "max_types cap exceeded");  // cannot register beyond the cap
        }
        // --- resource / custom validation (004 undeclared resource, etc.).
        if (result<void> v = check(); !v) {
            report_.entries.push_back(
                {mode_ == Validation::Strict ? Severity::Error : Severity::Warning, v.error().code,
                 m.key, v.error().detail});
            if (mode_ == Validation::Strict) return std::unexpected<error>(v.error());
            // Relaxed: fall through and register the degraded (quarantined) actor.
        }
        m.index = static_cast<std::uint16_t>(records_.size());
        by_key_.emplace(m.key, m.index);
        records_.push_back(m);
        return m.index;
    }

    // --- Lookups (008 §Runtime — flat arrays, no string keys on any path) ---------------------
    [[nodiscard]] const ActorMetadata* find(TypeKey k) const noexcept {
        const auto it = by_key_.find(k);
        return it == by_key_.end() ? nullptr : &records_[it->second];
    }
    [[nodiscard]] const ActorMetadata* at(std::uint16_t index) const noexcept {
        return index < records_.size() ? &records_[index] : nullptr;
    }

    // --- Factory (008 §Metadata compilation ConstructFn) --------------------------------------
    // Construct a fresh actor by key (heap-owned by the caller; free via `destroy`).
    [[nodiscard]] void* construct(TypeKey k) const {
        const ActorMetadata* m = find(k);
        return (m && m->construct) ? m->construct() : nullptr;
    }
    void destroy(TypeKey k, void* self) const noexcept {
        const ActorMetadata* m = find(k);
        if (m && m->destroy) m->destroy(self);
    }
    // Reconstruct an actor's state in place (fresh) — the 007 Restart factory.
    void reconstruct(TypeKey k, void* self) const noexcept {
        const ActorMetadata* m = find(k);
        if (m) m->reconstruct(self);
    }

    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    [[nodiscard]] Validation mode() const noexcept { return mode_; }
    [[nodiscard]] const ValidationReport& report() const noexcept { return report_; }

private:
    Validation mode_;
    std::uint32_t max_types_;
    std::vector<ActorMetadata> records_;                  // by dense type_index (008)
    std::unordered_map<TypeKey, std::uint16_t> by_key_;   // TypeKey → type_index (wire/storage lookup)
    ValidationReport report_;
};

}  // namespace quark
