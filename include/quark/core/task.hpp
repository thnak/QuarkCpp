// Implements 001-Actor-Execution-Model §Hybrid handler execution — the minimal async return
// type. A `quark::task<>` handler selects the async execution mode at compile time (ADR-007
// jump table) and drives the activation-suspend hand-off (ADR-015 §Parked).
//
// SCOPE (001): this is deliberately minimal — a lazy, single-frame coroutine whose ONLY job is
// (a) to be detectable as the async mode, and (b) to expose the suspension hand-off point where
// the executor parks the activation. The FULL coroutine admission / reentrancy / quiescence /
// frame-pool machinery is owned by 015 — see `detach()` for the seam boundary.
#pragma once

#include <coroutine>
#include <exception>
#include <utility>

namespace quark {

// Only the `void` result is needed by the 001 seam; a value-returning `ask` task<T> is 006/ADR-007.
template <class T = void>
class task;

template <>
class task<void> {
public:
    struct promise_type {
        [[nodiscard]] task<void> get_return_object() noexcept {
            return task<void>{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        // Lazy: the frame is created suspended so the executor starts it explicitly (one `resume`)
        // and can observe whether the FIRST co_await actually suspended (park) or the body ran to
        // completion inline (no co_await ⇒ drain proceeds without parking).
        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        // Suspend at the final point too, so the frame survives for the executor's `done()` probe
        // and reclamation — the executor owns destruction (never self-destroying).
        [[nodiscard]] std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        // ADR-009 handler-boundary guard (007): an async handler that throws is CONTAINED here,
        // never `std::terminate`. The coroutine machinery routes the throw to `unhandled_exception`
        // (the frame then runs to `final_suspend`, so it is `done()` and reclaimable). The executor
        // probes `faulted()` after each resume (via `async_frame_faulted`) and drives supervision —
        // the fault becomes a VALUE (a failed reply / dead-letter), exactly as the spec requires
        // ("the exception surfaces when the task<> completes"). Capturing the exception_ptr is a
        // COLD, async-frame-only cost; it never touches the sync zero-cost hot path (ADR-009 F1/F2).
        void unhandled_exception() noexcept { fault_ = std::current_exception(); }
        [[nodiscard]] bool faulted() const noexcept { return static_cast<bool>(fault_); }

        std::exception_ptr fault_{};  // null unless the handler threw (cold; async path only)
    };

    task() noexcept = default;
    explicit task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}

    task(const task&) = delete;
    task& operator=(const task&) = delete;
    task(task&& other) noexcept : h_(std::exchange(other.h_, {})) {}
    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (h_) h_.destroy();
            h_ = std::exchange(other.h_, {});
        }
        return *this;
    }
    ~task() {
        if (h_) h_.destroy();
    }

    // 015 SUSPENSION SEAM. Hand the (not-yet-started) frame to the executor as a type-erased
    // handle; the executor starts it (one `resume`), parks the activation on the first co_await,
    // and owns the frame's lifetime and reclamation thereafter (015). After `detach()` this task
    // no longer owns the frame.
    [[nodiscard]] std::coroutine_handle<> detach() noexcept { return std::exchange(h_, {}); }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(h_); }

private:
    std::coroutine_handle<promise_type> h_{};
};

// ADR-009 async-fault probe. Every async handler returns `quark::task<>`, so a type-erased async
// handler frame's promise is ALWAYS `task<void>::promise_type` — the typed handle is reconstructible
// from its address (the dispatch layer only ever detaches `task<void>` frames). The executor calls
// this after starting/resuming an async frame to learn whether the handler threw (surfaced at
// completion, per 007) and must be routed through the supervision guard instead of completed clean.
[[nodiscard]] inline bool async_frame_faulted(std::coroutine_handle<> h) noexcept {
    if (!h) return false;
    return std::coroutine_handle<task<>::promise_type>::from_address(h.address()).promise().faulted();
}

}  // namespace quark
