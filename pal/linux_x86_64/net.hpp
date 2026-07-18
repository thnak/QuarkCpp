// Implements 019-Platform-Abstraction-Layer §2 (async I/O event loop + sockets) — the Linux/x86-64
// backend. This is the ONE place cross-node networking touches OS APIs (`<sys/*>`, `<netinet/*>`,
// `<unistd.h>`): 019 §"The one rule" — subsystem code includes PAL headers, never OS headers. The
// default TCP transport (010, include/quark/net/tcp_transport.hpp) is written against THIS surface,
// never against a raw syscall.
//
// PROACTOR SHAPE, EPOLL BACKEND (019 §"Completion (proactor) I/O model, not readiness"): 019 mandates
// a completion-based interface so it maps cleanly onto io_uring/IOCP; readiness backends (epoll/kqueue)
// "emulate completion by performing the operation on readiness." That is exactly what this backend does:
// `IoContext` is an epoll reactor whose registered handlers PERFORM the recv/send/accept when the fd is
// ready, then hand the *result* upward — the transport above sees frames arrive and buffers drain, not
// raw readiness. The io_uring backend (native completion) slots behind the same IoContext surface with
// zero transport changes (019 §"Per-OS backends": io_uring-vs-epoll is a build option, not runtime).
//
// THREADING: an IoContext is single-threaded — `run()` owns the loop, and add_fd/mod_fd/del_fd/
// post_after are called ONLY from the loop thread (inside handlers). Cross-thread work is marshalled in
// via `post()` (mutex-guarded queue + eventfd wake), so all fd/socket state stays on one thread with no
// locking on the I/O path. 019 §"Not in the PAL": the memory model is std::atomic's job, not the PAL's;
// the only shared state here (the post queue + stop flag) uses std::mutex / std::atomic explicitly.
//
// Fallible calls return std::expected<T, std::error_code> (019 §"The surface"): OS errno is normalized
// into std::error_code via std::system_category(); the transport maps that to quark::errc::unavailable
// (010 delivery table) at its edge. This header is Linux-only and #error-guards other platforms.
#pragma once

#if !defined(__linux__)
#error "pal/linux_x86_64/net.hpp is the Linux backend of the 019 PAL socket seam; \
other OSes need their own backend (kqueue/IOCP). Do not include it elsewhere."
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <mutex>
#include <map>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pal/linux_x86_64/clock.hpp"  // BootClock — the monotonic clock timers ride (018/019)

namespace quark::pal {

// The canonical PAL monotonic instant in ns (018 CLOCK_BOOTTIME class) — the clock the event-loop timer
// queue schedules against, the same domain as every engine deadline.
[[nodiscard]] inline std::int64_t mono_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(BootClock::now().time_since_epoch())
        .count();
}

// errno → std::error_code, the 019 normalization. Captured immediately after a failing syscall.
[[nodiscard]] inline std::error_code last_error() noexcept {
    return std::error_code(errno, std::system_category());
}

// The "operation would block" sentinel a non-blocking recv/send returns — a normal readiness edge, NOT
// a failure. Callers compare against this to decide "stop, wait for the next EPOLLIN/EPOLLOUT".
[[nodiscard]] inline std::error_code would_block() noexcept {
    return std::make_error_code(std::errc::operation_would_block);
}

// --- IPv4 locator codec ---------------------------------------------------------------------------
// 021's Endpoint carries an opaque `addr` (std::uint64_t) + `port`; the PAL interprets it. Here the low
// 32 bits of `addr` are an IPv4 address in HOST byte order (a.b.c.d → (a<<24)|(b<<16)|(c<<8)|d), which
// keeps the on-wire Endpoint endian-clean and human-obvious. IPv6 is a deferred widening of the locator.
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

// --- socket primitives (non-blocking, close-on-exec) ----------------------------------------------

inline std::expected<void, std::error_code> set_nonblocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return std::unexpected(last_error());
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return std::unexpected(last_error());
    return {};
}

// Disable Nagle: actor frames are small and latency-sensitive (023), so we never want the kernel to
// coalesce-and-delay. Best-effort — a failure here is not fatal to correctness.
inline void set_nodelay(int fd) noexcept {
    const int one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

inline void close_fd(int fd) noexcept {
    if (fd >= 0) ::close(fd);
}

// Open a listening socket bound to (addr,port). port==0 ⇒ an ephemeral port the OS picks (read it back
// with local_port). Returns the listening fd (non-blocking, SO_REUSEADDR).
[[nodiscard]] inline std::expected<int, std::error_code> tcp_listen(std::uint64_t addr,
                                                                    std::uint16_t port,
                                                                    int backlog = 128) noexcept {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::unexpected(last_error());
    const int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    const ::sockaddr_in sa = to_sockaddr_in(addr, port);
    if (::bind(fd, reinterpret_cast<const ::sockaddr*>(&sa), sizeof(sa)) < 0) {
        const auto e = last_error();
        close_fd(fd);
        return std::unexpected(e);
    }
    if (::listen(fd, backlog) < 0) {
        const auto e = last_error();
        close_fd(fd);
        return std::unexpected(e);
    }
    if (auto r = set_nonblocking(fd); !r) {
        close_fd(fd);
        return std::unexpected(r.error());
    }
    return fd;
}

// The actual port a (possibly ephemeral) listener bound to — so a test can bind port 0 and dial back.
[[nodiscard]] inline std::expected<std::uint16_t, std::error_code> local_port(int fd) noexcept {
    ::sockaddr_in sa{};
    ::socklen_t len = sizeof(sa);
    if (::getsockname(fd, reinterpret_cast<::sockaddr*>(&sa), &len) < 0)
        return std::unexpected(last_error());
    return ::ntohs(sa.sin_port);
}

// Begin a non-blocking connect. Returns the fd immediately; the connect is IN PROGRESS (EINPROGRESS is
// expected and NOT an error) — completion is signalled by EPOLLOUT, at which point connect_result(fd)
// reports success/failure. A synchronous connect (loopback) may complete here; that is also success.
[[nodiscard]] inline std::expected<int, std::error_code> tcp_connect(std::uint64_t addr,
                                                                     std::uint16_t port) noexcept {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::unexpected(last_error());
    if (auto r = set_nonblocking(fd); !r) {
        close_fd(fd);
        return std::unexpected(r.error());
    }
    set_nodelay(fd);
    const ::sockaddr_in sa = to_sockaddr_in(addr, port);
    if (::connect(fd, reinterpret_cast<const ::sockaddr*>(&sa), sizeof(sa)) < 0 && errno != EINPROGRESS) {
        const auto e = last_error();
        close_fd(fd);
        return std::unexpected(e);
    }
    return fd;
}

// After EPOLLOUT on a connecting socket: was the connect successful? SO_ERROR holds the verdict.
[[nodiscard]] inline std::expected<void, std::error_code> connect_result(int fd) noexcept {
    int err = 0;
    ::socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return std::unexpected(last_error());
    if (err != 0) return std::unexpected(std::error_code(err, std::system_category()));
    return {};
}

// Accept one pending connection (non-blocking listener). Returns the accepted fd (non-blocking,
// nodelay), or would_block() when the backlog is drained (the normal loop-exit condition).
[[nodiscard]] inline std::expected<int, std::error_code> accept_one(int lfd) noexcept {
    const int fd = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return std::unexpected(would_block());
        return std::unexpected(last_error());
    }
    set_nodelay(fd);
    return fd;
}

// Non-blocking recv. Ok(n>0) = bytes read; Ok(0) = orderly peer close (EOF); would_block() = nothing
// ready now; any other error = broken connection.
[[nodiscard]] inline std::expected<std::size_t, std::error_code> recv_some(int fd, std::byte* buf,
                                                                           std::size_t n) noexcept {
    const ::ssize_t r = ::recv(fd, buf, n, 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return std::unexpected(would_block());
        return std::unexpected(last_error());
    }
    return static_cast<std::size_t>(r);
}

// Non-blocking send. MSG_NOSIGNAL: a write to a peer-closed socket returns EPIPE instead of raising
// SIGPIPE (which would kill the process). Ok(n) = bytes accepted (may be < n); would_block() = the
// kernel send buffer is full, retry on the next EPOLLOUT.
[[nodiscard]] inline std::expected<std::size_t, std::error_code> send_some(int fd, const std::byte* buf,
                                                                           std::size_t n) noexcept {
    const ::ssize_t r = ::send(fd, buf, n, MSG_NOSIGNAL);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return std::unexpected(would_block());
        return std::unexpected(last_error());
    }
    return static_cast<std::size_t>(r);
}

// ================================================================================================
// IoContext — the epoll reactor + cross-thread task queue + monotonic timer queue. One per node's
// transport. All fd/timer mutation happens on the run() thread; post()/stop() are thread-safe entry
// points that wake the loop via an eventfd.
// ================================================================================================
class IoContext {
public:
    using ReadyHandler = std::function<void(std::uint32_t /*epoll events*/)>;

    IoContext() {
        epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
        wakefd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        // The wake eventfd is level-triggered EPOLLIN; a post() writes it, run() drains + runs tasks.
        ::epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = wakefd_;
        (void)::epoll_ctl(epfd_, EPOLL_CTL_ADD, wakefd_, &ev);
    }

    ~IoContext() {
        close_fd(wakefd_);
        close_fd(epfd_);
    }

    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;

    // --- loop-thread-only: fd interest registry -------------------------------------------------
    bool add_fd(int fd, std::uint32_t events, ReadyHandler handler) {
        ::epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) return false;
        handlers_[fd] = std::move(handler);
        return true;
    }
    bool mod_fd(int fd, std::uint32_t events) {
        ::epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        return ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
    }
    void del_fd(int fd) {
        (void)::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        handlers_.erase(fd);
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
        constexpr int kMaxEvents = 64;
        ::epoll_event evs[kMaxEvents];
        while (!stop_.load(std::memory_order_acquire)) {
            const int timeout_ms = next_timeout_ms();
            const int n = ::epoll_wait(epfd_, evs, kMaxEvents, timeout_ms);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;  // epoll fd is broken — nothing sane to do but exit the loop
            }
            fire_due_timers();
            for (int i = 0; i < n; ++i) {
                const int fd = evs[i].data.fd;
                if (fd == wakefd_) {
                    drain_wake();
                    run_posted();
                    continue;
                }
                // Copy the handler before invoking: a handler may del_fd(itself) (e.g. close on EOF),
                // which would erase the map entry mid-call — the copy keeps it alive for this dispatch.
                const auto it = handlers_.find(fd);
                if (it == handlers_.end()) continue;  // a prior handler in this batch closed this fd
                ReadyHandler h = it->second;
                h(evs[i].events);
            }
        }
    }

private:
    void wake() noexcept {
        const std::uint64_t one = 1;
        // A full eventfd counter (2^64-2) or a transient EAGAIN is harmless — the pending byte already
        // there will still wake the loop. Consume the result explicitly (warn_unused_result on write).
        if (::write(wakefd_, &one, sizeof(one)) < 0) { /* already-signalled / EAGAIN: fine */ }
    }
    void drain_wake() noexcept {
        std::uint64_t v = 0;
        while (::read(wakefd_, &v, sizeof(v)) > 0) {
        }  // fully drain (EFD_NONBLOCK ⇒ read returns -1/EAGAIN when empty)
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

    int epfd_ = -1;
    int wakefd_ = -1;
    std::atomic<bool> stop_{false};

    std::mutex qmu_;
    std::vector<std::function<void()>> queue_;  // cross-thread tasks (guarded)

    std::unordered_map<int, ReadyHandler> handlers_;      // fd → readiness handler (loop-thread only)
    std::multimap<std::int64_t, std::function<void()>> timers_;  // fire_ns → callback (loop-thread only)
};

}  // namespace quark::pal
