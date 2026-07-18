// Implements 001-Actor-Execution-Model §Cancellation — the ambient per-message context handed to
// a running handler. Carries the `std::stop_token` an in-flight (`Running`) handler observes for
// cooperative cancellation (001), plus the deadline/trace metadata copied from the descriptor.
//
// SCOPE (001): this is the minimal envelope. ADR-007 carries the full ambient context (principal,
// headers, override vs inherited) via a descriptor `ctx_` pointer into the payload-arena envelope;
// the running-handler `stop_token` is backed by an ACTIVATION-POOLED `std::stop_source` (ADR-009),
// never a per-message allocation. 001 holds one source per activation (see Activation).
#pragma once

#include <cstdint>
#include <stop_token>

#include "quark/core/principal.hpp"  // Principal — the ambient security identity (020 §3)

namespace quark {

struct MessageContext {
    std::stop_token stop{};       // 001 cooperative cancellation for the in-flight handler
    std::int64_t deadline_ns = 0;  // steady-clock deadline (011); 0 = none
    std::uint64_t trace_id = 0;    // trace correlation id (009)
    Principal principal{};         // 020 ambient identity; default = anonymous (single-node pays nothing)

    [[nodiscard]] bool stop_requested() const noexcept { return stop.stop_requested(); }
    [[nodiscard]] const std::stop_token& stop_token() const noexcept { return stop; }

    // 020 §3: the security principal that authorized this message. Rides the causal chain like the
    // deadline; a purely intra-process tell leaves it anonymous. Available in a handler with no plumbing.
    [[nodiscard]] const Principal& principal_ref() const noexcept { return principal; }
};

namespace detail {

// Ambient current-context propagation (006 + 009 + 018). The MessageContext of the handler CURRENTLY
// running on this thread is published here so a `tell`/`ask` the handler issues auto-inherits its
// trace correlation id (009) and deadline (018) — WITHOUT the discrete-message API taking a ctx arg.
// The Activation publishes it (via AmbientContextScope) around every handler dispatch/resume; the
// router (actor_ref.hpp LocalRouter::post_message) reads it. Null outside a handler (bootstrap sends
// start a fresh trace / no deadline). thread_local because a handler runs on exactly one lane.
inline thread_local const MessageContext* tl_current_ctx = nullptr;

// RAII: publish `c` as the ambient context for a handler invocation, restoring the previous on exit
// (so a tell that itself runs a handler — never, but nested drives in tests — stacks correctly).
class AmbientContextScope {
public:
    explicit AmbientContextScope(const MessageContext& c) noexcept : prev_(tl_current_ctx) {
        tl_current_ctx = &c;
    }
    ~AmbientContextScope() { tl_current_ctx = prev_; }
    AmbientContextScope(const AmbientContextScope&) = delete;
    AmbientContextScope& operator=(const AmbientContextScope&) = delete;

private:
    const MessageContext* prev_;
};

}  // namespace detail

}  // namespace quark
