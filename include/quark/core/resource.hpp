// Implements 004-Resources — the actor resource model with ZERO dynamic resolution on the hot
// path. Resources are declared as member fields whose *type* encodes a lifetime (`Cached<T>`,
// `PerMessage<T>`, `Ambient<T>`); the plan is materialized ONCE at activation (cold) by walking a
// `ResourceScope`, after which every hot-path access is a plain pointer read — never a container
// walk (004 §Rules "No dynamic resolution while draining").
//
// FOUNDATION SEAM (004 §Declaring resources / ADR-007): member-field declarations are the chosen
// ergonomics. std C++23 cannot enumerate an actor's members, so — exactly as `dispatch.hpp` does
// for the message Protocol — the wiring list is explicit: an actor exposes its resources to the
// one-time activation wire pass via `quark::wire_resources(scope, a_, b_, ...)`. The per-message
// product acquire/release (004 §Rules "Per-message resources come from factories", ADR-009) is the
// `ProductGuard` RAII below; the engine hook that constructs it in the guarded pre-handler region
// is reported to 007 (not edited into activation.hpp here).
#pragma once

#include <cstdint>
#include <optional>
#include <stop_token>
#include <tuple>
#include <utility>
#include <vector>

#include "quark/core/config.hpp"
#include "quark/core/deadline.hpp"  // 018: the canonical quark::Deadline (ambient injection uses it)
#include "quark/core/error.hpp"
#include "quark/core/message_context.hpp"

namespace quark {

// ---- Lifetimes (004 §Lifetimes) --------------------------------------------------------------
// Determines where a resource is stored and when it is resolved. Longer-lived scopes are strictly
// cheaper: Singleton/Node/Shard/Activation are all resolved once (cold) and read as a pointer on
// the hot path; Message (Ambient) values are never "resolved" — they ride the MessageContext.
enum class ResourceLifetime : std::uint8_t {
    Singleton,   // Engine-wide, resolved once at startup (config, metrics registry)
    Node,        // Per node, resolved once per node (machine-wide connection pool)
    Shard,       // Per shard, resolved once per shard (shard-local cache, allocator handle)
    Activation,  // Per activation, resolved once when the actor activates (logger, pool handle)
    Message,     // Ambient — carried with the message in the MessageContext, never resolved
};

namespace detail {

// RTTI-free stable type identity (CONVENTIONS: no typeid/dynamic_cast in core TUs). The address of
// a per-type static byte is unique and stable for the program's lifetime — a compile-time type key.
template <class T>
struct resource_type_tag {
    static constexpr char id = 0;
};
template <class T>
inline constexpr const void* resource_type_key = &resource_type_tag<T>::id;

}  // namespace detail

// ---- ResourceScope (004) — the COLD wiring container -----------------------------------------
// A type-keyed set of provided resource instances for a wiring pass. Populated at startup / node /
// shard / activation time (cold), then handed to the one-time wire pass. It is NEVER consulted
// while a message is processed — `Cached<>`/`PerMessage<>` copy out the resolved handle and read it
// directly thereafter. Resolution is a linear scan over a handful of entries; the allocation lives
// entirely on the cold path.
class ResourceScope {
public:
    // Register a resource instance under its type. `instance` must outlive every actor wired from
    // this scope (its lifetime is governed by the scope level, 004 §Lifetimes).
    template <class T>
    void provide(T& instance, ResourceLifetime lifetime) {
        entries_.push_back(Entry{detail::resource_type_key<T>, &instance, lifetime});
    }

    // Resolve the instance for type T. `errc::validation` (004/008) if no provider was declared —
    // this is the "undeclared resource is a validation error" contract, surfaced through
    // `quark::result` and checked at wire time (cold), never on the hot path.
    template <class T>
    [[nodiscard]] result<T*> resolve() const {
        for (const Entry& e : entries_) {
            if (e.type_key == detail::resource_type_key<T>) {
                return static_cast<T*>(e.instance);
            }
        }
        return fail(errc::validation, "resource not provided in scope (undeclared)");
    }

    // The recorded lifetime for a provided type (cold; for validation/observability).
    template <class T>
    [[nodiscard]] result<ResourceLifetime> lifetime_of() const {
        for (const Entry& e : entries_) {
            if (e.type_key == detail::resource_type_key<T>) return e.lifetime;
        }
        return fail(errc::validation, "resource not provided in scope (undeclared)");
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        const void* type_key;
        void* instance;
        ResourceLifetime lifetime;
    };
    std::vector<Entry> entries_{};
};

// ---- Cached<T> (004) — an Activation/Node/Shard/Singleton resource, resolved ONCE -------------
// Declared as an actor member. `wire()` runs once at activation (cold) and copies the resolved
// pointer in; every subsequent access (`get()`, `operator->`, `operator*`) is a single pointer
// read with NO lookup, NO allocation, NO branch on the hot path. This is how 004's "zero dynamic
// resolution while a message is processed" is achieved for heavy handles (logger, pool, cache).
template <class T>
class Cached {
public:
    Cached() = default;

    // Cold, one-time wiring (004 §Declaring resources; runs at activation / metadata materialize).
    [[nodiscard]] result<void> wire(const ResourceScope& scope) {
        result<T*> r = scope.resolve<T>();
        if (!r) return std::unexpected(r.error());
        ptr_ = *r;
        return {};
    }

    // --- Hot path: pure pointer reads (no resolution) -----------------------------------------
    [[nodiscard]] QUARK_ALWAYS_INLINE T& get() const noexcept { return *ptr_; }
    [[nodiscard]] QUARK_ALWAYS_INLINE T* operator->() const noexcept { return ptr_; }
    [[nodiscard]] QUARK_ALWAYS_INLINE T& operator*() const noexcept { return *ptr_; }
    [[nodiscard]] QUARK_ALWAYS_INLINE bool resolved() const noexcept { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

// ---- Factory<T> (004) — the CACHED activation resource that yields per-message products --------
// Type-erased, trivially-copyable producer. The factory itself is an Activation-scoped `Cached<>`
// resource (provided into the scope); its *product* is the per-message resource. The producer may
// signal failure by returning `std::unexpected(...)` OR by throwing — 004 handles both uniformly at
// the 007 boundary (see ProductGuard).
template <class T>
class Factory {
public:
    using fn_t = result<T> (*)(void* state);

    Factory() = default;
    Factory(fn_t fn, void* state) noexcept : fn_(fn), state_(state) {}

    [[nodiscard]] result<T> create() const { return fn_(state_); }
    [[nodiscard]] bool valid() const noexcept { return fn_ != nullptr; }

private:
    fn_t fn_ = nullptr;
    void* state_ = nullptr;
};

// Build a Factory<T> from a callable `F` returning `result<T>`. `f` must outlive the factory (it is
// an activation-scoped resource — e.g. a connection-pool handle whose `operator()` checks out a
// session). Cold-path helper.
template <class T, class F>
[[nodiscard]] Factory<T> make_factory(F& f) noexcept {
    return Factory<T>{+[](void* state) -> result<T> { return (*static_cast<F*>(state))(); }, &f};
}

// ---- PerMessage<T> (004) — a product with single-message lifetime -----------------------------
// Declared as an actor member. `wire()` caches the Factory<T> (cold, activation). `acquire()` runs
// in the guarded pre-handler region (004 §Rules / ADR-009): it produces and CHECKS the product
// BEFORE the handler body, so a factory that returns `unexpected` and one that throws are handled
// uniformly and the handler never runs with a null/degraded product. `get()` on the hot path
// returns the already-acquired product (no factory call, no lookup). The product's own destructor
// (RAII) releases any external checkout at message end — driven by ProductGuard.
template <class T>
class PerMessage {
public:
    PerMessage() = default;

    // Cold, one-time wiring: resolve (cache) the Activation-scoped factory.
    [[nodiscard]] result<void> wire(const ResourceScope& scope) {
        result<Factory<T>*> r = scope.resolve<Factory<T>>();
        if (!r) return std::unexpected(r.error());
        factory_ = *r;
        return {};
    }

    // Guarded pre-handler acquire (004): produce + check the product before dispatch. On the
    // `unexpected` channel this returns the error; a THROWING factory propagates and is caught by
    // the 007 boundary (ProductGuard::acquire is not noexcept for exactly this reason).
    [[nodiscard]] result<void> acquire() {
        if (!factory_ || !factory_->valid()) return fail(errc::internal, "PerMessage factory unwired");
        result<T> r = factory_->create();
        if (!r) return std::unexpected(r.error());
        product_.emplace(std::move(*r));
        return {};
    }

    // Hot path: the pre-acquired product (no resolution). Matches the 004 example `session_.get()`.
    [[nodiscard]] QUARK_ALWAYS_INLINE T& get() noexcept { return *product_; }
    [[nodiscard]] QUARK_ALWAYS_INLINE bool acquired() const noexcept { return product_.has_value(); }

    // End-of-message release — the product's RAII destructor runs here (004 §Rules).
    void release() noexcept { product_.reset(); }

private:
    const Factory<T>* factory_ = nullptr;
    std::optional<T> product_{};
};

// ---- Ambient<T> (004 §Message context) — values that ride the MessageContext ------------------
// Never "resolved": read directly from the MessageContext the handler already holds. Specialize
// `ambient_traits<T>` to expose a field. The two strong types below name the numeric fields the
// current MessageContext carries; `std::stop_token` is exposed directly.
//
// SEAM (004): the fuller MessageContext (principal, header_view) is not yet in the foundation
// header — see the reported hook. Those become additional `ambient_traits` specializations with no
// change to Ambient<>.
// (The ambient `Deadline` is the canonical 018 `quark::Deadline` from deadline.hpp — a handler that
// injects `Ambient<Deadline>` gets its full expired()/remaining() API, not a bare ns wrapper.)
struct TraceId {
    std::uint64_t value = 0;  // trace correlation id (009)
};

template <class T>
struct ambient_traits;  // primary left undefined — only known ambient fields specialize it

template <>
struct ambient_traits<std::stop_token> {
    [[nodiscard]] static const std::stop_token& get(const MessageContext& c) noexcept { return c.stop; }
};
template <>
struct ambient_traits<Deadline> {
    [[nodiscard]] static Deadline get(const MessageContext& c) noexcept {
        return Deadline::at_ns(c.deadline_ns);
    }
};
template <>
struct ambient_traits<TraceId> {
    [[nodiscard]] static TraceId get(const MessageContext& c) noexcept { return TraceId{c.trace_id}; }
};

template <class T>
struct Ambient {
    // Read the ambient value from the message's context — no lookup, no resolution (004).
    [[nodiscard]] QUARK_ALWAYS_INLINE static decltype(auto) get(const MessageContext& ctx) noexcept {
        return ambient_traits<T>::get(ctx);
    }
};

// ---- One-time activation wire pass (004 §Declaring resources) --------------------------------
// The explicit-list analogue of the dispatch Protocol (std C++23 cannot walk members). An actor
// exposes its Cached<>/PerMessage<> members here; each is wired once (cold). Short-circuits on the
// first validation error and returns it, so an undeclared resource fails the whole wiring with
// `errc::validation` — checked at activation, never on the hot path.
template <class... Rs>
[[nodiscard]] result<void> wire_resources(const ResourceScope& scope, Rs&... rs) {
    result<void> err{};  // success by default
    const bool ok = (... && [&] {
        result<void> r = rs.wire(scope);
        if (!r) {
            err = std::unexpected(r.error());
            return false;
        }
        return true;
    }());
    (void)ok;
    return err;
}

// ---- ProductGuard (004 §Rules / ADR-009) — the guarded per-message region ---------------------
// RAII over an actor's PerMessage<> members. The engine constructs it in the pre-handler guarded
// region: `acquire()` produces+checks every product BEFORE the handler body (uniform failure on
// both the `unexpected` and throwing channels — a throw unwinds through the guard, releasing any
// already-acquired products via their RAII destructors). On any failure, dispatch is skipped and
// the 007 boundary fires. On success the handler runs, then ~ProductGuard releases all products
// (their RAII destructors run) at message end.
template <class... Ps>
class ProductGuard {
public:
    explicit ProductGuard(Ps&... ps) noexcept : ps_(ps...) {}
    ProductGuard(const ProductGuard&) = delete;
    ProductGuard& operator=(const ProductGuard&) = delete;

    // Produce + check all products before the handler. Short-circuits on the first failure; a
    // throwing factory propagates (the 007 boundary catches it) and already-acquired products are
    // released by this guard's destructor as the stack unwinds.
    [[nodiscard]] result<void> acquire() {
        return std::apply(
            [](auto&... p) -> result<void> {
                result<void> err{};
                const bool ok = (... && [&] {
                    result<void> r = p.acquire();
                    if (!r) {
                        err = std::unexpected(r.error());
                        return false;
                    }
                    return true;
                }());
                (void)ok;
                return err;
            },
            ps_);
    }

    ~ProductGuard() {
        std::apply([](auto&... p) noexcept { (p.release(), ...); }, ps_);
    }

private:
    std::tuple<Ps&...> ps_;
};

}  // namespace quark
