// Implements 001-Actor-Execution-Model §Hybrid handler execution / ADR-015 §Parked — the ambient
// seam a co_await'ing awaiter (ReplyCell, detail/reply_cell.hpp) uses to route its resume through
// the engine's exec-state-gated Activation::complete_parked() instead of a raw coroutine_handle::
// resume(), so a reply that resolves on a DIFFERENT worker's lane never touches the asking actor's
// state directly (001 single-executor).
//
// Deliberately separate from detail::tl_current_ctx / AmbientContextScope (message_context.hpp):
// that one carries the user-facing MessageContext (principal/deadline/trace, handed to handlers BY
// VALUE, part of the public API surface); this one is internal-only plumbing (type-erased engine +
// Schedulable pointers) that must never leak into a handler's view.
#pragma once

namespace quark::detail {

// Type-erased "how to complete_parked() the activation currently draining on THIS lane" — the
// codebase's established DeadLetterSink/EscalationSink/ReconstructSink function-pointer-seam idiom
// (no virtual, no RTTI). Set by Engine::run_activation (engine.hpp) around every drain_step() call,
// for Sequential/governed-Sequential activations ONLY (Reentrant actors use a separate per-frame
// completion mechanism, not the single-slot parked_frame_/complete_parked() seam — an inactive sink
// here for a Reentrant activation is deliberate, not an oversight; see engine.hpp's run_activation).
// Captured by ReplyCell::suspend() at the moment a co_await commits to suspending — MUST be
// snapshotted there, not read lazily later: by the time a reply resolves (on a DIFFERENT thread/
// lane) this thread-local reflects THAT lane's own, unrelated activation, if any.
struct ParkedResumeSink {
    void (*fn)(void* engine, void* schedulable) noexcept = nullptr;
    void* engine = nullptr;
    void* schedulable = nullptr;

    [[nodiscard]] bool active() const noexcept { return fn != nullptr; }
    void operator()() const noexcept { fn(engine, schedulable); }
};

// Null outside a Sequential/governed-Sequential handler dispatch — bootstrap code, a Reentrant
// activation's dispatch, or a bare/standalone coroutine driven without a real Engine (e.g. existing
// unit tests driving a bare Activation directly). A ReplyCell whose suspend() captured an inactive
// sink falls back to the pre-existing direct h.resume() — unchanged from before this seam existed.
inline thread_local ParkedResumeSink tl_current_parked_resume{};

// RAII: publish `s` as the ambient parked-resume sink for the duration of a drain_step() call,
// restoring the previous value on exit (mirrors detail::AmbientContextScope's shape exactly).
class ParkedResumeScope {
public:
    explicit ParkedResumeScope(ParkedResumeSink s) noexcept : prev_(tl_current_parked_resume) {
        tl_current_parked_resume = s;
    }
    ~ParkedResumeScope() { tl_current_parked_resume = prev_; }
    ParkedResumeScope(const ParkedResumeScope&) = delete;
    ParkedResumeScope& operator=(const ParkedResumeScope&) = delete;

private:
    ParkedResumeSink prev_;
};

}  // namespace quark::detail
