// Implements 001-Actor-Execution-Model §Hybrid handler execution — the minimal async return
// type. A `quark::task<>` handler selects the async execution mode at compile time (ADR-007
// jump table) and drives the activation-suspend hand-off (ADR-015 §Parked).
//
// SCOPE (001): `task<>`/`task<void>` (below) is deliberately minimal — a lazy, single-frame
// coroutine whose ONLY job is (a) to be detectable as the async mode, and (b) to expose the
// suspension hand-off point where the executor parks the activation. The FULL coroutine
// admission / reentrancy / quiescence / frame-pool machinery is owned by 015 — see `detach()`
// for the seam boundary. `task<void>` remains the ONLY type ever `detach()`ed to the executor
// or fed to `async_frame_faulted()`/`async_frame_fault_ptr()` below — dispatch.hpp's
// `async_handler` concept checks the EXACT type `task<>`, so a handler declared to return
// `task<T!=void>` fails to compile, structurally, with no extra guard needed here.
//
// `task<T>` for T != void (ADR-047): a genuinely awaitable INNER coroutine — an ordinary async
// function (e.g. `task<result<Foo>> compute(...)`) that a handler's own `task<void>` frame (or
// another `task<T>`) `co_await`s to get a `T` back. Lazy like `task<void>`; `task<T>` IS its own
// awaiter, and completion hands control back to the awaiter via symmetric transfer (same
// thread/stack — no scheduler hop). A throw anywhere in a nested `task<T>` is caught by
// `unhandled_exception()` (never `std::terminate`, ADR-009 D1) and rethrown at the awaiting
// `await_resume()` — the same idiom `task<void>`'s `fault_`/`faulted()`/`fault_ptr()` already
// uses, just rethrown instead of merely probed. A `task<T>` dropped without ever being awaited
// is always safe to destroy: it is suspended at `initial_suspend` (the body never started).
//
// Cross-lane resume routing: when a nested `task<T>` genuinely parks on a cross-actor primitive
// (an `ask`'s `ReplyCell`), the reply may resolve on a DIFFERENT worker's lane. Before ADR-047,
// `Activation::complete_parked()` unconditionally resumed the activation's own top-level
// `task<void>` handle — correct when nothing nests, but silently wrong (data corruption / a
// heap-use-after-free) the instant a `task<T>` several layers deep is the frame that actually
// needs to run next. ADR-047 threads the LEAF handle `ReplyCell::suspend()` captured through
// `detail::ParkedResumeSink`/`ReplyCell::resolve()`/`Activation::complete_parked()` (see those
// files) so the correct frame resumes; `parked_frame_` remains the sole signal for
// done()/fault/reclaim, unchanged.
#pragma once

#include <coroutine>
#include <concepts>
#include <exception>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace quark {

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
        // ADR-009 residual risk #6: lets the executor classify WHAT faulted (e.g. a `ResourceFailure`,
        // resource.hpp, vs. an ordinary handler throw) via rethrow+catch, mirroring the classification
        // already available for a SYNC throw at the dispatch call site (activation.hpp).
        [[nodiscard]] std::exception_ptr fault_ptr() const noexcept { return fault_; }

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

// --- task<T>, T != void (ADR-047): a nested, awaitable, value-returning coroutine. See the
// banner comment above for the full contract. NEVER detach()ed to the executor — task<void>
// above remains the only type that seam ever touches. -------------------------------------
template <class T>
class task {
public:
    struct promise_type {
        [[nodiscard]] task get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        // Lazy, same discipline as task<void>: the frame starts suspended so it only ever runs
        // when something actually awaits it (task<T>::await_suspend below resumes it).
        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }

        // Symmetric transfer back to whoever is awaiting this task (or std::noop_coroutine() if
        // this frame was never awaited, e.g. dropped mid-flight during teardown) — completion
        // hands control straight back on the SAME thread/stack, no extra resume() call.
        struct FinalAwaiter {
            [[nodiscard]] bool await_ready() noexcept { return false; }
            [[nodiscard]] std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept {
                std::coroutine_handle<> cont = h.promise().continuation_;
                return cont ? cont : std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        [[nodiscard]] FinalAwaiter final_suspend() noexcept { return {}; }

        // Storage for the co_returned T lives inline in the promise (zero extra heap allocation
        // beyond the compiler-managed frame itself, matching ADR-009's existing "COLD, async-
        // frame-only cost" framing for task<void>) — an aligned byte buffer + construct_at/
        // destroy_at gated by has_value_, the same manually-managed-storage idiom
        // detail::ReplyCell<T> already uses for its own non-trivial `result<R>` payload.
        template <class U>
            requires std::constructible_from<T, U&&>
        void return_value(U&& v) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            std::construct_at(value_ptr(), std::forward<U>(v));
            has_value_ = true;
        }
        // ADR-009 D1 containment, extended to nested task<T>: a throw is caught here — never
        // std::terminate — and observable only by rethrowing it at THIS frame's own
        // await_resume(); it propagates as an ordinary C++ exception up through however many
        // task<T> layers await this one, same as any other exception would.
        void unhandled_exception() noexcept { fault_ = std::current_exception(); }

        // Route through void* (not reinterpret_cast) so the aligned access carries no
        // -Wcast-align noise; std::launder for the placement-new'd object (mirrors
        // detail::ReplyCell<R>::value_ptr()'s identical idiom).
        [[nodiscard]] T* value_ptr() noexcept {
            return std::launder(static_cast<T*>(static_cast<void*>(store_)));
        }

        promise_type() noexcept = default;
        ~promise_type() {
            if (has_value_) value_ptr()->~T();
        }

        std::coroutine_handle<> continuation_{};  // who to symmetric-transfer to at final_suspend
        std::exception_ptr fault_{};               // set iff the body threw (cold; never touches
                                                     // the sync hot path)
        alignas(T) unsigned char store_[sizeof(T)];
        bool has_value_ = false;
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
    // Safe whether or not this task was ever awaited: unawaited, the frame is still suspended at
    // initial_suspend (its body never ran) — destroying a coroutine suspended at any suspension
    // point, including the initial one, is well-defined and leaks nothing.
    ~task() {
        if (h_) h_.destroy();
    }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(h_); }

    // co_await protocol — task<T> IS its own awaiter (ADR-047 D1). `await_suspend` records the
    // awaiting coroutine as this task's continuation and returns THIS task's own handle: the
    // language calls .resume() on the returned handle for us (symmetric transfer), starting the
    // (lazy) frame in the exact same step, with no scheduler hop and no extra stack frame.
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
        h_.promise().continuation_ = awaiting;
        return h_;
    }
    [[nodiscard]] T await_resume() {
        promise_type& p = h_.promise();
        if (p.fault_) std::rethrow_exception(p.fault_);
        return std::move(*p.value_ptr());
    }

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

// The captured `exception_ptr` behind a faulted async frame (null if `h` did not fault). Lets the
// executor classify WHAT faulted (ADR-009 residual risk #6 — e.g. `resource.hpp`'s `ResourceFailure`
// vs. an ordinary handler throw) via rethrow+catch, the same way a SYNC throw is classified at its
// call site.
[[nodiscard]] inline std::exception_ptr async_frame_fault_ptr(std::coroutine_handle<> h) noexcept {
    if (!h) return nullptr;
    return std::coroutine_handle<task<>::promise_type>::from_address(h.address()).promise().fault_ptr();
}

}  // namespace quark
