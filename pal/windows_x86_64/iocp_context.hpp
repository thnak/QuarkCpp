// Implements 019-Platform-Abstraction-Layer §2 (async I/O event loop) — Windows IOCP reactor, ADR-059.
//
// WHAT THIS IS. A drop-in for the WSAPoll IoContext (same public shape/threading contract:
// add_fd/mod_fd/del_fd/post_after loop-thread-only; post()/stop() thread-safe; run() on one thread)
// whose single blocking primitive is GetQueuedCompletionStatusEx on an I/O completion port. Socket
// READINESS is delivered through the port with the AFD-poll technique (IOCTL_AFD_POLL against
// \Device\Afd, the mechanism wepoll/libuv use for epoll emulation on Windows): one-shot, level-
// triggered poll requests whose completions land on the IOCP. Cross-thread post()/stop() use
// PostQueuedCompletionStatus — no loopback-socket wake pair, no per-iteration O(n) pollfd rebuild.
//
// WHAT THIS IS NOT. It is not a true proactor (overlapped WSARecv/WSASend with buffers owned by the
// kernel): the TcpTransport is written against a readiness surface (recv_some/send_some after a
// ready callback), and the PAL seam keeps that shape. A proactor needs a transport-side change (ADR-059
// stage 3, not done here).
//
// CAVEAT. IOCTL_AFD_POLL / \Device\Afd are not formally documented by Microsoft; they are stable in
// practice (wepoll, libuv, mio, Go's netpoll history), but this is a real risk the ADR records. The
// loop falls back by construction: if the AFD device cannot be opened the constructor records
// `ok() == false` and run() is a no-op — build with QUARK_WIN_IOCP=OFF to use WSAPoll.
#pragma once

#if !defined(_WIN32)
#error "pal/windows_x86_64/iocp_context.hpp is the Windows IOCP reactor; include via pal/net.hpp."
#endif

#include <winternl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quark::pal {

class IocpContext {
public:
    using ReadyHandler = std::function<void(std::uint32_t /*EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP*/)>;

    IocpContext() {
        ensure_winsock();
        iocp_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
        if (nt != nullptr) {
            nt_create_file_ = reinterpret_cast<NtCreateFileFn>(::GetProcAddress(nt, "NtCreateFile"));
            nt_device_io_ = reinterpret_cast<NtDeviceIoControlFileFn>(
                ::GetProcAddress(nt, "NtDeviceIoControlFile"));
        }
        if (iocp_ == nullptr || nt_create_file_ == nullptr || nt_device_io_ == nullptr) return;

        static const wchar_t kName[] = L"\\Device\\Afd\\Quark";
        UNICODE_STRING us{};
        us.Buffer = const_cast<PWSTR>(kName);
        us.Length = static_cast<USHORT>(sizeof(kName) - sizeof(wchar_t));
        us.MaximumLength = static_cast<USHORT>(sizeof(kName));
        OBJECT_ATTRIBUTES oa{};
        InitializeObjectAttributes(&oa, &us, 0, nullptr, nullptr);
        IO_STATUS_BLOCK iosb{};
        HANDLE afd = nullptr;
        const NTSTATUS st = nt_create_file_(&afd, SYNCHRONIZE, &oa, &iosb, nullptr, 0,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, kFileOpen, 0,
                                            nullptr, 0);
        if (st != 0 || afd == nullptr) return;
        if (::CreateIoCompletionPort(afd, iocp_, kKeySock, 0) == nullptr) {
            ::CloseHandle(afd);
            return;
        }
        ::SetFileCompletionNotificationModes(afd, FILE_SKIP_SET_EVENT_ON_HANDLE);
        afd_ = afd;
    }

    ~IocpContext() {
        // Cancel every in-flight poll and wait for the kernel to hand each IOSB back before freeing —
        // the kernel writes into Sock::iosb/info, so freeing while pending would be a use-after-free.
        for (auto& [fd, s] : socks_) {
            s->closed = true;
            s->handler = nullptr;
            zombies_.insert(s);
        }
        socks_.clear();
        for (Sock* z : zombies_) {
            if (z->pending && afd_ != nullptr) ::CancelIoEx(afd_, reinterpret_cast<LPOVERLAPPED>(&z->iosb));
        }
        for (int spins = 0; spins < 200 && pending_count() > 0; ++spins) {
            drain_for_teardown(10);
        }
        for (Sock* z : zombies_) {
            if (!z->pending) delete z;  // a still-pending one (kernel never returned it) is leaked, not freed
        }
        zombies_.clear();
        if (afd_ != nullptr) ::CloseHandle(afd_);
        if (iocp_ != nullptr) ::CloseHandle(iocp_);
    }

    IocpContext(const IocpContext&) = delete;
    IocpContext& operator=(const IocpContext&) = delete;

    [[nodiscard]] bool ok() const noexcept { return afd_ != nullptr; }

    // --- loop-thread-only: fd interest registry -------------------------------------------------
    bool add_fd(fd_t fd, std::uint32_t events, ReadyHandler handler) {
        if (!ok()) return false;
        if (auto it = socks_.find(fd); it != socks_.end()) del_fd(fd);
        auto* s = new Sock{};
        s->sock = fd;
        s->base = reinterpret_cast<SOCKET>(base_handle(fd));
        s->interest = events;
        s->handler = std::move(handler);
        socks_[fd] = s;
        arm(s);
        return true;
    }
    bool mod_fd(fd_t fd, std::uint32_t events) {
        const auto it = socks_.find(fd);
        if (it == socks_.end()) return false;
        Sock* s = it->second;
        if (s->interest == events) return true;
        const ULONG before = afd_mask(s->interest);
        s->interest = events;
        // A pending poll was submitted with the OLD mask; cancel it so the completion re-arms with the new one.
        if (s->pending && afd_mask(events) != before) {
            ::CancelIoEx(afd_, reinterpret_cast<LPOVERLAPPED>(&s->iosb));
        }
        return true;
    }
    void del_fd(fd_t fd) {
        const auto it = socks_.find(fd);
        if (it == socks_.end()) return;
        Sock* s = it->second;
        socks_.erase(it);
        s->closed = true;
        s->handler = nullptr;
        if (s->pending) {
            zombies_.insert(s);  // freed when its (cancelled) completion is dequeued
            ::CancelIoEx(afd_, reinterpret_cast<LPOVERLAPPED>(&s->iosb));
        } else if (!s->dispatching) {
            delete s;
        }  // else: the dispatcher deletes it when the handler returns
    }

    // --- loop-thread-only: schedule a callback after `delay_ns` ----------------------------------
    void post_after(std::int64_t delay_ns, std::function<void()> fn) {
        timers_.emplace(mono_ns() + (delay_ns < 0 ? 0 : delay_ns), std::move(fn));
    }

    // --- thread-safe ----------------------------------------------------------------------------
    void post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> g(qmu_);
            queue_.push_back(std::move(fn));
        }
        wake();
    }
    void stop() noexcept {
        stop_.store(true, std::memory_order_release);
        wake();
    }

    // --- the loop. Runs on ONE thread until stop(). ---------------------------------------------
    void run() {
        if (!ok()) return;
        constexpr ULONG kBatch = 64;
        OVERLAPPED_ENTRY entries[kBatch];
        while (!stop_.load(std::memory_order_acquire)) {
            ULONG n = 0;
            const BOOL okq = ::GetQueuedCompletionStatusEx(
                iocp_, entries, kBatch, &n, static_cast<DWORD>(next_timeout_ms()), FALSE);
            // Capture the error BEFORE running timers: timer callbacks make syscalls that clobber it
            // (a timeout misread as a hard failure silently killed the loop — found by tcp_relay_fallback_test).
            const DWORD err = okq ? 0 : ::GetLastError();
            fire_due_timers();
            if (!okq) {
                if (err == WAIT_TIMEOUT) continue;
                break;  // the port itself is broken — nothing sane to do but leave the loop
            }
            for (ULONG i = 0; i < n; ++i) {
                if (entries[i].lpOverlapped == nullptr) {  // wake packet (post()/stop())
                    run_posted();
                    continue;
                }
                complete(reinterpret_cast<Sock*>(entries[i].lpOverlapped));  // iosb is Sock's first member
            }
        }
    }

private:
    // ---- AFD / NT plumbing (declared locally: not in the public SDK headers) ----------------------
    using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                            PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using NtDeviceIoControlFileFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK,
                                                     ULONG, PVOID, ULONG, PVOID, ULONG);
    static constexpr ULONG kIoctlAfdPoll = 0x00012024;
    static constexpr ULONG kPollReceive = 0x0001, kPollSend = 0x0004, kPollDisconnect = 0x0008,
                           kPollAbort = 0x0010, kPollLocalClose = 0x0020, kPollAccept = 0x0080,
                           kPollConnectFail = 0x0100;
    static constexpr NTSTATUS kStatusPending = 0x103;
    static constexpr NTSTATUS kStatusCancelled = static_cast<NTSTATUS>(0xC0000120);
    static constexpr ULONG_PTR kKeySock = 1;
    static constexpr ULONG kSioBaseHandle = 0x48000022;  // SIO_BASE_HANDLE (mswsock.h)
    static constexpr ULONG kFileOpen = 1;

    struct AfdPollHandleInfo {
        HANDLE handle;
        ULONG events;
        NTSTATUS status;
    };
    struct AfdPollInfo {
        LARGE_INTEGER timeout;
        ULONG number_of_handles;
        ULONG exclusive;
        AfdPollHandleInfo handles[1];
    };

    // iosb MUST stay the first member: the completion packet carries &iosb as its OVERLAPPED*.
    struct Sock {
        IO_STATUS_BLOCK iosb{};
        AfdPollInfo info{};
        fd_t sock = invalid_fd;
        SOCKET base = INVALID_SOCKET;
        std::uint32_t interest = 0;
        ReadyHandler handler;
        bool pending = false;
        bool closed = false;
        bool dispatching = false;
    };

    static HANDLE base_handle(fd_t s) {
        SOCKET base = INVALID_SOCKET;
        DWORD bytes = 0;
        if (::WSAIoctl(s, kSioBaseHandle, nullptr, 0, &base, sizeof(base), &bytes, nullptr, nullptr) != 0) {
            return reinterpret_cast<HANDLE>(s);
        }
        return reinterpret_cast<HANDLE>(base);
    }

    static ULONG afd_mask(std::uint32_t interest) noexcept {
        ULONG m = kPollAbort | kPollLocalClose | kPollConnectFail;  // errors are always reported (epoll parity)
        if (interest & EPOLLIN) m |= kPollReceive | kPollAccept | kPollDisconnect;
        if (interest & EPOLLOUT) m |= kPollSend;
        return m;
    }

    void arm(Sock* s) {
        if (s->pending || s->closed) return;
        s->info = AfdPollInfo{};
        s->info.timeout.QuadPart = INT64_MAX;
        s->info.number_of_handles = 1;
        s->info.exclusive = FALSE;
        s->info.handles[0].handle = reinterpret_cast<HANDLE>(s->base);
        s->info.handles[0].events = afd_mask(s->interest);
        s->iosb.Status = static_cast<NTSTATUS>(kStatusPending);
        s->pending = true;
        const NTSTATUS st = nt_device_io_(afd_, nullptr, nullptr, &s->iosb, &s->iosb, kIoctlAfdPoll,
                                          &s->info, sizeof(s->info), &s->info, sizeof(s->info));
        if (st != 0 && st != kStatusPending) {
            // Submission failed outright: no packet will come, so synthesize one carrying the failure.
            s->iosb.Status = st;
            s->info.handles[0].events = 0;
            ::PostQueuedCompletionStatus(iocp_, 0, kKeySock, reinterpret_cast<LPOVERLAPPED>(&s->iosb));
        }
    }

    [[nodiscard]] static std::uint32_t to_epoll(ULONG afd_events) noexcept {
        std::uint32_t ev = 0;
        if (afd_events & (kPollReceive | kPollAccept | kPollDisconnect)) ev |= EPOLLIN;  // DISCONNECT: recv()==0
        if (afd_events & kPollSend) ev |= EPOLLOUT;
        if (afd_events & kPollConnectFail) ev |= EPOLLERR;
        if (afd_events & kPollAbort) ev |= EPOLLERR | EPOLLHUP;
        if (afd_events & kPollLocalClose) ev |= EPOLLHUP;
        return ev;
    }

    void complete(Sock* s) {
        s->pending = false;
        if (s->closed) {  // del_fd'd while the poll was in flight: this is its cancelled/finished packet
            zombies_.erase(s);
            delete s;
            return;
        }
        std::uint32_t ev = 0;
        if (s->iosb.Status == 0) {
            ev = to_epoll(s->info.handles[0].events);
        } else if (s->iosb.Status != kStatusCancelled) {
            ev = EPOLLERR;  // the poll itself failed (e.g. socket handle invalid)
        }
        // Interest may have changed since submission: deliver only what is currently asked for (errors always).
        ev &= (s->interest | EPOLLERR | EPOLLHUP);
        if (ev != 0 && s->handler) {
            s->dispatching = true;
            ReadyHandler h = s->handler;  // copy: the handler may del_fd(itself)
            h(ev);
            s->dispatching = false;
            if (s->closed) {  // del_fd ran inside the handler
                if (!s->pending) {
                    zombies_.erase(s);
                    delete s;
                }
                return;
            }
        }
        // Level-triggered: re-arm; if the socket is still ready the new poll completes immediately.
        // A hard error (EPOLLERR with a failed poll) is not re-armed here — the handler is expected to
        // close the fd, which del_fd's it. If it did not, re-arming would spin, so only re-arm on a
        // healthy status.
        if (s->iosb.Status == 0 || s->iosb.Status == kStatusCancelled) arm(s);
    }

    std::size_t pending_count() const noexcept {
        std::size_t n = 0;
        for (Sock* z : zombies_) n += z->pending ? 1 : 0;
        return n;
    }
    void drain_for_teardown(DWORD timeout_ms) {
        OVERLAPPED_ENTRY e[16];
        ULONG n = 0;
        if (!::GetQueuedCompletionStatusEx(iocp_, e, 16, &n, timeout_ms, FALSE)) return;
        for (ULONG i = 0; i < n; ++i) {
            if (e[i].lpOverlapped == nullptr) continue;
            auto* s = reinterpret_cast<Sock*>(e[i].lpOverlapped);
            s->pending = false;
            zombies_.erase(s);
            delete s;
        }
    }

    void wake() noexcept {
        if (iocp_ != nullptr) ::PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);
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
        if (timers_.empty()) return -1;  // INFINITE (DWORD cast of -1)
        const std::int64_t now = mono_ns();
        const std::int64_t due = timers_.begin()->first;
        if (due <= now) return 0;
        const std::int64_t ms = (due - now + 999'999) / 1'000'000;
        return ms > 1'000'000'000 ? 1'000'000'000 : static_cast<int>(ms);
    }
    void fire_due_timers() {
        const std::int64_t now = mono_ns();
        std::vector<std::function<void()>> due;
        while (!timers_.empty() && timers_.begin()->first <= now) {
            due.push_back(std::move(timers_.begin()->second));
            timers_.erase(timers_.begin());
        }
        for (auto& fn : due) fn();
    }

    HANDLE iocp_ = nullptr;
    HANDLE afd_ = nullptr;
    NtCreateFileFn nt_create_file_ = nullptr;
    NtDeviceIoControlFileFn nt_device_io_ = nullptr;
    std::atomic<bool> stop_{false};

    std::mutex qmu_;
    std::vector<std::function<void()>> queue_;

    std::unordered_map<fd_t, Sock*> socks_;      // loop-thread only
    std::unordered_set<Sock*> zombies_;           // del_fd'd with a poll still in flight
    std::multimap<std::int64_t, std::function<void()>> timers_;
};

}  // namespace quark::pal
