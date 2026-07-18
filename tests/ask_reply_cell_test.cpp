// Tests ADR-007 §5 (reply routing) — the ReplyCell win-arbitration handshake in isolation (no
// engine): both race orders (awaiter-suspends-first and responder-resolves-first) deliver the reply
// exactly once; the monotonic generation fences a stale resolve (ABA); an unanswered ask fails the
// cell so the awaiter never hangs. Drives a standalone test coroutine so the awaiter path is real.
#include <coroutine>
#include <cstdio>

#include "quark/core/actor_ref.hpp"

using namespace quark;

namespace {

// A minimal eager-driven test coroutine (not on an activation — this unit-tests the cell only).
struct Coro {
    struct promise_type {
        Coro get_return_object() noexcept {
            return Coro{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
    std::coroutine_handle<promise_type> h{};
    explicit Coro(std::coroutine_handle<promise_type> hh) noexcept : h(hh) {}
    Coro(Coro&& o) noexcept : h(std::exchange(o.h, {})) {}
    ~Coro() { if (h) h.destroy(); }
    void resume() { h.resume(); }
    [[nodiscard]] bool done() const { return h.done(); }
};

// A NAMED coroutine (not a capturing lambda — a lambda-coroutine's closure temporary would be
// destroyed while the coroutine is suspended, dangling `this`). `fut` is passed BY VALUE so the
// frame owns it across the suspend point; results are written through pointers.
Coro await_into(AskFuture<int> fut, result<int>* out, bool* done) {
    *out = co_await std::move(fut);
    *done = true;
    co_return;
}

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;
    detail::ReplyCellPool<int> pool(16);

    // ---- Order 1: awaiter SUSPENDS first, responder resolves second (responder resumes it). ----
    {
        auto lease = pool.acquire();
        Responder<int> resp(lease.cell, lease.gen);
        result<int> out;
        bool done = false;
        Coro coro = await_into(AskFuture<int>(lease.cell, &pool), &out, &done);
        coro.resume();  // runs to co_await; cell empty ⇒ suspends
        check(!done, "order1: awaiter suspended (cell empty)", ok);
        resp(42);       // win-arbitration: responder observes kWaiter ⇒ resumes the continuation
        check(done, "order1: continuation resumed by responder", ok);
        check(out.has_value() && out.value() == 42, "order1: correct reply delivered", ok);
    }

    // ---- Order 2: responder RESOLVES first, awaiter awaits second (no suspend, value present). --
    {
        auto lease = pool.acquire();
        Responder<int> resp(lease.cell, lease.gen);
        resp(7);        // resolve before the await
        result<int> out;
        bool done = false;
        Coro coro = await_into(AskFuture<int>(lease.cell, &pool), &out, &done);
        coro.resume();  // await_ready() true ⇒ completes without suspending
        check(done, "order2: completed without suspending (value already present)", ok);
        check(out.has_value() && out.value() == 7, "order2: correct reply delivered", ok);
    }

    // ---- Monotonic-generation fence: a stale-gen resolve is a no-op (ABA). ----
    {
        detail::ReplyCell<int> c;
        const std::uint64_t g = c.arm();
        c.resolve(g + 999, result<int>(5));  // wrong generation ⇒ dropped
        check(!c.ready(), "stale-gen resolve is a no-op (ABA fence)", ok);
        c.resolve(g, result<int>(5));         // matching generation ⇒ lands
        check(c.ready(), "matching-gen resolve lands", ok);
        check(c.take().value() == 5, "correct value after gen-matched resolve", ok);
    }

    // ---- Reply-before-teardown at the cell level: an unanswered Responder fails the cell. ----
    {
        auto lease = pool.acquire();
        AskFuture<int> fut(lease.cell, &pool);
        {
            Responder<int> resp(lease.cell, lease.gen);
            (void)resp;  // never answered — destructor must fail the cell
        }
        result<int> r = fut.wait();  // must return (an error), NOT hang
        check(!r.has_value() && r.error().code == errc::supervised_stop,
              "unanswered ask fails the cell (no caller hang)", ok);
    }

    std::printf("ask_reply_cell_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
