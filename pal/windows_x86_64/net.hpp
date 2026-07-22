// Implements 019-Platform-Abstraction-Layer §2 (async I/O event loop + sockets) — the Windows/x86-64
// backend. Mirrors pal/linux_x86_64/net.hpp's PUBLIC SURFACE exactly (same `fd_t`/`invalid_fd`,
// `mono_ns`/`last_error`/`would_block`, `EPOLLIN` et al., IPv4 codec, socket primitives, and the
// `IoContext` class shape) so include/quark/net/tcp_transport.hpp — written against that surface, not
// a raw OS API — compiles and behaves identically over this backend without any changes above the
// seam (019 §"The one rule").
//
// READINESS MULTIPLEXER: WSAPoll, not IOCP (see decisions/plan rationale) — the direct Windows
// analogue of `poll()`, so `IoContext` stays a readiness reactor (register interest, perform the op
// when ready) exactly like the epoll backend, instead of a differently-shaped completion proactor. A
// true IOCP proactor is a documented future upgrade behind this same class shape.
//
// WSAPoll takes an explicit fd array on every call (no persistent kernel-side registration like
// epoll_ctl), so `add_fd`/`mod_fd`/`del_fd` here just update in-memory maps; `run()` rebuilds the
// WSAPOLLFD array from them each iteration.
//
// WAKE PRIMITIVE: Windows has no `eventfd`. `post()`/`stop()` wake the loop via a connected loopback
// TCP pair (bind+listen+connect+accept on 127.0.0.1) — the standard portable "self-pipe" substitute
// for `socketpair()`, which Windows does not provide for AF_INET.
//
// THREADING / error normalization: identical contract to the epoll backend (see that file's banner).
#pragma once

#if !defined(_WIN32)
#error "pal/windows_x86_64/net.hpp is the Windows backend of the 019 PAL socket seam; \
other OSes need their own backend (epoll/kqueue). Do not include it elsewhere."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pal/windows_x86_64/clock.hpp"  // BootClock — the monotonic clock timers ride (018/019)

// Portable readiness-interest bitmask, matching pal/linux_x86_64/net.hpp's epoll.h values exactly.
// tcp_transport.hpp uses these UNQUALIFIED (they are C macros on Linux too — the epoll.h convention),
// so this backend defines them as global macros rather than namespaced constants; only the numeric
// values matter (they gate quark::pal::IoContext's own interest tracking, never cross an OS boundary).
#ifndef EPOLLIN
#define EPOLLIN 0x001
#endif
#ifndef EPOLLOUT
#define EPOLLOUT 0x004
#endif
#ifndef EPOLLERR
#define EPOLLERR 0x008
#endif
#ifndef EPOLLHUP
#define EPOLLHUP 0x010
#endif

namespace quark::pal {

// Portable fd handle type (019 §"The one rule") — SOCKET here (UINT_PTR, NOT interchangeable with
// POSIX's `int`), matching pal/linux_x86_64/net.hpp's `fd_t` alias. tcp_transport.hpp and IoContext
// are written against `fd_t`, never a raw `int` or `SOCKET`.
using fd_t = SOCKET;
inline const fd_t invalid_fd = INVALID_SOCKET;

// One-time WSAStartup, lazily on first socket use (function-local static — thread-safe init since
// C++11). No matching WSACleanup: this is a long-lived process-wide resource other code in the same
// process may also depend on, so we deliberately never tear it down.
inline void ensure_winsock() noexcept {
    static const int rc = [] {
        WSADATA wsa{};
        return ::WSAStartup(MAKEWORD(2, 2), &wsa);
    }();
    (void)rc;
}

// The canonical PAL monotonic instant in ns (018 CLOCK_BOOTTIME class) — same contract as the epoll
// backend's mono_ns(), backed by this OS's BootClock (pal/windows_x86_64/clock.hpp).
[[nodiscard]] inline std::int64_t mono_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(BootClock::now().time_since_epoch())
        .count();
}

// WSAGetLastError() → std::error_code, normalized into system_category (019's normalization). Both
// last_error() and would_block() below deliberately share system_category so `r.error() ==
// pal::would_block()` compares reliably (Windows' system_category is a distinct object from
// generic_category — unlike the errno-numbered categories POSIX backends can get away with comparing
// across, same-category same-value is the only comparison this relies on).
[[nodiscard]] inline std::error_code last_error() noexcept {
    return std::error_code(::WSAGetLastError(), std::system_category());
}

[[nodiscard]] inline std::error_code would_block() noexcept {
    return std::error_code(WSAEWOULDBLOCK, std::system_category());
}

// --- IPv4 locator codec ---------------------------------------------------------------------------
// Identical contract to the epoll backend: low 32 bits of `addr` are an IPv4 address in HOST byte
// order. `htons`/`htonl`/`sockaddr_in` come from <winsock2.h> — same names, same layout as POSIX.
[[nodiscard]] inline constexpr std::uint64_t ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c,
                                                  std::uint8_t d) noexcept {
    return (static_cast<std::uint64_t>(a) << 24) | (static_cast<std::uint64_t>(b) << 16) |
           (static_cast<std::uint64_t>(c) << 8) | static_cast<std::uint64_t>(d);
}
inline constexpr std::uint64_t ipv4_loopback = ipv4(127, 0, 0, 1);

[[nodiscard]] inline ::sockaddr_in to_sockaddr_in(std::uint64_t addr, std::uint16_t port) noexcept {
    ::sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = ::htons(port);
    sa.sin_addr.s_addr = ::htonl(static_cast<std::uint32_t>(addr & 0xFFFF'FFFFULL));
    return sa;
}

// --- socket primitives (non-blocking) ---------------------------------------------------------

inline std::expected<void, std::error_code> set_nonblocking(fd_t s) noexcept {
    u_long mode = 1;
    if (::ioctlsocket(s, FIONBIO, &mode) != 0) return std::unexpected(last_error());
    return {};
}

// Disable Nagle: actor frames are small and latency-sensitive (023). Best-effort — a failure here is
// not fatal to correctness. (No MSG_NOSIGNAL equivalent needed on Windows: sockets never raise SIGPIPE
// here — that is a POSIX-only concern the epoll backend's send_some() guards against.)
inline void set_nodelay(fd_t s) noexcept {
    const int one = 1;
    (void)::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

inline void close_fd(fd_t s) noexcept {
    if (s != invalid_fd) ::closesocket(s);
}

// Open a listening socket bound to (addr,port). port==0 ⇒ an ephemeral port the OS picks (read it back
// with local_port). Returns the listening socket (non-blocking, SO_REUSEADDR).
[[nodiscard]] inline std::expected<fd_t, std::error_code> tcp_listen(std::uint64_t addr,
                                                                     std::uint16_t port,
                                                                     int backlog = 128) noexcept {
    ensure_winsock();
    const fd_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == invalid_fd) return std::unexpected(last_error());
    const int one = 1;
    (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    const ::sockaddr_in sa = to_sockaddr_in(addr, port);
    if (::bind(s, reinterpret_cast<const ::sockaddr*>(&sa), sizeof(sa)) != 0) {
        const auto e = last_error();
        close_fd(s);
        return std::unexpected(e);
    }
    if (::listen(s, backlog) != 0) {
        const auto e = last_error();
        close_fd(s);
        return std::unexpected(e);
    }
    if (auto r = set_nonblocking(s); !r) {
        close_fd(s);
        return std::unexpected(r.error());
    }
    return s;
}

// The actual port a (possibly ephemeral) listener bound to — so a test can bind port 0 and dial back.
[[nodiscard]] inline std::expected<std::uint16_t, std::error_code> local_port(fd_t s) noexcept {
    ::sockaddr_in sa{};
    int len = sizeof(sa);
    if (::getsockname(s, reinterpret_cast<::sockaddr*>(&sa), &len) != 0)
        return std::unexpected(last_error());
    return ::ntohs(sa.sin_port);
}

// Begin a non-blocking connect. Returns the socket immediately; the connect is IN PROGRESS
// (WSAEWOULDBLOCK is expected and NOT an error — Windows' non-blocking-connect-in-progress signal,
// the analogue of POSIX EINPROGRESS) — completion is signalled by writable readiness, at which point
// connect_result(s) reports success/failure. A synchronous connect (loopback) may complete here too.
[[nodiscard]] inline std::expected<fd_t, std::error_code> tcp_connect(std::uint64_t addr,
                                                                      std::uint16_t port) noexcept {
    ensure_winsock();
    const fd_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == invalid_fd) return std::unexpected(last_error());
    if (auto r = set_nonblocking(s); !r) {
        close_fd(s);
        return std::unexpected(r.error());
    }
    set_nodelay(s);
    const ::sockaddr_in sa = to_sockaddr_in(addr, port);
    if (::connect(s, reinterpret_cast<const ::sockaddr*>(&sa), sizeof(sa)) != 0) {
        const int err = ::WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            close_fd(s);
            return std::unexpected(std::error_code(err, std::system_category()));
        }
    }
    return s;
}

// After writable-readiness on a connecting socket: was the connect successful? SO_ERROR holds the
// verdict (same semantics as POSIX).
[[nodiscard]] inline std::expected<void, std::error_code> connect_result(fd_t s) noexcept {
    int err = 0;
    int len = sizeof(err);
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0)
        return std::unexpected(last_error());
    if (err != 0) return std::unexpected(std::error_code(err, std::system_category()));
    return {};
}

// Accept one pending connection (non-blocking listener). Returns the accepted socket (non-blocking,
// nodelay), or would_block() when the backlog is drained (the normal loop-exit condition).
[[nodiscard]] inline std::expected<fd_t, std::error_code> accept_one(fd_t lfd) noexcept {
    const fd_t s = ::accept(lfd, nullptr, nullptr);
    if (s == invalid_fd) {
        const int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return std::unexpected(would_block());
        return std::unexpected(std::error_code(err, std::system_category()));
    }
    if (auto r = set_nonblocking(s); !r) {
        close_fd(s);
        return std::unexpected(r.error());
    }
    set_nodelay(s);
    return s;
}

// Non-blocking recv. Ok(n>0) = bytes read; Ok(0) = orderly peer close (EOF); would_block() = nothing
// ready now; any other error = broken connection.
[[nodiscard]] inline std::expected<std::size_t, std::error_code> recv_some(fd_t s, std::byte* buf,
                                                                           std::size_t n) noexcept {
    const int r = ::recv(s, reinterpret_cast<char*>(buf), static_cast<int>(n), 0);
    if (r < 0) {
        const int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return std::unexpected(would_block());
        return std::unexpected(std::error_code(err, std::system_category()));
    }
    return static_cast<std::size_t>(r);
}

// Non-blocking send. Ok(n) = bytes accepted (may be < n); would_block() = the kernel send buffer is
// full, retry on the next writable-readiness.
[[nodiscard]] inline std::expected<std::size_t, std::error_code> send_some(fd_t s, const std::byte* buf,
                                                                           std::size_t n) noexcept {
    const int r = ::send(s, reinterpret_cast<const char*>(buf), static_cast<int>(n), 0);
    if (r < 0) {
        const int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return std::unexpected(would_block());
        return std::unexpected(std::error_code(err, std::system_category()));
    }
    return static_cast<std::size_t>(r);
}

// A connected loopback TCP pair — the portable socketpair() substitute AF_INET lacks on Windows.
// Both ends are non-blocking; used solely as the IoContext wake primitive (a single byte written to
// `write_end` becomes POLLRDNORM-readiness on `read_end`, exactly like the epoll backend's eventfd).
struct WakePair { fd_t read_end; fd_t write_end; };

[[nodiscard]] inline WakePair make_wake_pair() noexcept {
    ensure_winsock();
    const fd_t lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    ::sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(lfd, reinterpret_cast<::sockaddr*>(&addr), sizeof(addr));
    ::listen(lfd, 1);
    int len = sizeof(addr);
    ::getsockname(lfd, reinterpret_cast<::sockaddr*>(&addr), &len);

    const fd_t wfd = ::socket(AF_INET, SOCK_STREAM, 0);
    ::connect(wfd, reinterpret_cast<::sockaddr*>(&addr), sizeof(addr));
    const fd_t rfd = ::accept(lfd, nullptr, nullptr);
    close_fd(lfd);

    (void)set_nonblocking(rfd);
    (void)set_nonblocking(wfd);
    return WakePair{rfd, wfd};
}

// ================================================================================================
// IoContext — the WSAPoll reactor + cross-thread task queue + monotonic timer queue. Same public
// shape and threading contract as pal/linux_x86_64/net.hpp's IoContext (see that file's banner):
// add_fd/mod_fd/del_fd/post_after are loop-thread-only; post()/stop() are thread-safe.
// ================================================================================================
class IoContext {
public:
    using ReadyHandler = std::function<void(std::uint32_t /*EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP*/)>;

    IoContext() {
        ensure_winsock();
        const WakePair wp = make_wake_pair();
        wake_read_ = wp.read_end;
        wake_write_ = wp.write_end;
    }

    ~IoContext() {
        close_fd(wake_read_);
        close_fd(wake_write_);
    }

    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;

    // --- loop-thread-only: fd interest registry -------------------------------------------------
    // Unlike epoll_ctl, WSAPoll takes an explicit array every call — there is no persistent kernel-
    // side registration, so these three just maintain the in-memory maps run() rebuilds the array from.
    bool add_fd(fd_t fd, std::uint32_t events, ReadyHandler handler) {
        handlers_[fd] = std::move(handler);
        interest_[fd] = events;
        return true;
    }
    bool mod_fd(fd_t fd, std::uint32_t events) {
        if (handlers_.find(fd) == handlers_.end()) return false;
        interest_[fd] = events;
        return true;
    }
    void del_fd(fd_t fd) {
        handlers_.erase(fd);
        interest_.erase(fd);
    }

    // --- loop-thread-only: schedule a callback after `delay_ns` (jittered reconnect, 021) --------
    void post_after(std::int64_t delay_ns, std::function<void()> fn) {
        timers_.emplace(mono_ns() + (delay_ns < 0 ? 0 : delay_ns), std::move(fn));
    }

    // --- thread-safe: hand a task to the loop thread; wakes the loop --------------------------
    void post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> g(qmu_);
            queue_.push_back(std::move(fn));
        }
        wake();
    }

    // --- thread-safe: stop run() at the next iteration ------------------------------------------
    void stop() noexcept {
        stop_.store(true, std::memory_order_release);
        wake();
    }

    // --- the loop. Runs on ONE thread until stop(). ---------------------------------------------
    void run() {
        std::vector<WSAPOLLFD> fds;
        std::vector<fd_t> fd_order;  // fds[i] corresponds to fd_order[i] (WSAPOLLFD carries no payload)
        while (!stop_.load(std::memory_order_acquire)) {
            fds.clear();
            fd_order.clear();
            fds.reserve(interest_.size() + 1);
            fd_order.reserve(interest_.size() + 1);
            for (const auto& [fd, want] : interest_) {
                WSAPOLLFD pfd{};
                pfd.fd = fd;
                if (want & EPOLLIN) pfd.events |= POLLRDNORM;
                if (want & EPOLLOUT) pfd.events |= POLLWRNORM;
                fds.push_back(pfd);
                fd_order.push_back(fd);
            }
            {
                WSAPOLLFD wake_pfd{};
                wake_pfd.fd = wake_read_;
                wake_pfd.events = POLLRDNORM;
                fds.push_back(wake_pfd);
                fd_order.push_back(wake_read_);
            }

            const int timeout_ms = next_timeout_ms();
            const int n = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
            if (n < 0) break;  // WSAPoll itself is broken — nothing sane to do but exit the loop

            fire_due_timers();
            if (n == 0) continue;  // timeout, no readiness — re-check timers/stop and poll again

            for (std::size_t i = 0; i < fds.size(); ++i) {
                if (fds[i].revents == 0) continue;
                const fd_t fd = fd_order[i];
                if (fd == wake_read_) {
                    drain_wake();
                    run_posted();
                    continue;
                }
                std::uint32_t events = 0;
                if (fds[i].revents & POLLRDNORM) events |= EPOLLIN;
                if (fds[i].revents & POLLWRNORM) events |= EPOLLOUT;
                if (fds[i].revents & POLLERR) events |= EPOLLERR;
                if (fds[i].revents & POLLHUP) events |= EPOLLHUP;
                if (fds[i].revents & POLLNVAL) events |= EPOLLERR;
                // Copy the handler before invoking: a handler may del_fd(itself) (e.g. close on EOF),
                // which would erase the map entry mid-call — the copy keeps it alive for this dispatch.
                const auto it = handlers_.find(fd);
                if (it == handlers_.end()) continue;  // a prior handler in this batch closed this fd
                ReadyHandler h = it->second;
                h(events);
            }
        }
    }

private:
    void wake() noexcept {
        const char one = 1;
        (void)::send(wake_write_, &one, 1, 0);  // best-effort: a pending byte already wakes the loop
    }
    void drain_wake() noexcept {
        char buf[256];
        for (;;) {
            const int r = ::recv(wake_read_, buf, sizeof(buf), 0);
            if (r <= 0) break;  // WSAEWOULDBLOCK (fully drained) or the pair broke — either way, stop
        }
    }
    void run_posted() {
        std::vector<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> g(qmu_);
            batch.swap(queue_);
        }
        for (auto& fn : batch) fn();
    }

    [[nodiscard]] int next_timeout_ms() const {
        if (timers_.empty()) return -1;  // nothing scheduled — block until an fd or a post() wakes us
        const std::int64_t now = mono_ns();
        const std::int64_t due = timers_.begin()->first;
        if (due <= now) return 0;
        const std::int64_t ms = (due - now + 999'999) / 1'000'000;  // ceil to ms
        return ms > 1'000'000'000 ? 1'000'000'000 : static_cast<int>(ms);
    }
    void fire_due_timers() {
        const std::int64_t now = mono_ns();
        std::vector<std::function<void()>> due;
        while (!timers_.empty() && timers_.begin()->first <= now) {
            due.push_back(std::move(timers_.begin()->second));
            timers_.erase(timers_.begin());
        }
        for (auto& fn : due) fn();  // a timer may schedule another timer — safe, it lands past `now`
    }

    fd_t wake_read_ = invalid_fd;
    fd_t wake_write_ = invalid_fd;
    std::atomic<bool> stop_{false};

    std::mutex qmu_;
    std::vector<std::function<void()>> queue_;  // cross-thread tasks (guarded)

    std::unordered_map<fd_t, ReadyHandler> handlers_;    // fd → readiness handler (loop-thread only)
    std::unordered_map<fd_t, std::uint32_t> interest_;   // fd → EPOLLIN|EPOLLOUT mask (loop-thread only)
    std::multimap<std::int64_t, std::function<void()>> timers_;  // fire_ns → callback (loop-thread only)
};

}  // namespace quark::pal
