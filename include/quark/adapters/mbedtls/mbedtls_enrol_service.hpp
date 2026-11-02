// ADR-074 D4 / issue #36 (D4c step 4): the anchor's enrolment SERVICE. It takes sockets the transport hands off (TcpTransport::set_enrol_handoff) and runs
// one `EnrolAnchorSession` per socket on its OWN worker thread (a private IoContext), so the unauthenticated TLS handshakes and the CSR/HMAC work never run on
// the transport's I/O thread (020's crypto-isolation invariant): a flood of enrol hellos costs this thread, not member traffic.
//
// Admission (all cheap, decided on the caller's thread, the transport's I/O thread, before any TLS work):
//   * no unexpired, unconsumed ticket exists            -> refused (the caller closes the socket; zero bytes sent)
//   * `max_concurrent` enrolments already running        -> refused
//   * `max_per_ip` already running for the same address  -> refused
// A refused socket is counted, not audited per socket: an audit record per flood connection would turn the log into the amplifier. Every presentation that
// reaches `Enrollment::submit` IS audited by the Enrollment (ticket id, source, role, assigned id, status).
//
// Each enrolment has a hard deadline (`deadline_ns`, whole exchange, handshake included); the same worker also drives `Enrollment::expire` so tickets and
// stuck issuances age out without the application scheduling it.
//
// Threading: `handoff` may be called from one thread (the transport's I/O thread); everything else runs on the worker. Destruction stops the worker, closes
// every socket it still holds (including ones queued but not yet adopted) and destroys the sessions (a late custodian completion is then dropped).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pal/net.hpp"
#include "quark/adapters/mbedtls/mbedtls_enrol_session.hpp"
#include "quark/core/enrollment.hpp"
#include "quark/core/tls_identity.hpp"

namespace quark::adapters {

struct EnrolServiceConfig {
    std::size_t max_concurrent = 4;
    std::size_t max_per_ip = 2;
    std::int64_t deadline_ns = 30'000'000'000;        // the whole exchange
    std::int64_t expire_interval_ns = 1'000'000'000;  // Enrollment::expire cadence
};

class EnrolService {
public:
    using Clock = EnrolAnchorSession::Clock;

    // `own` is the anchor's member identity (the chain the joiner pins against); `enrollment` and the identity's key stay with the caller / this object.
    EnrolService(TlsIdentity own, enrol::Enrollment& enrollment, EnrolServiceConfig cfg, Clock now)
        : own_(std::move(own)), enr_(enrollment), cfg_(cfg), now_(std::move(now)) {
        worker_ = std::thread([this] {
            io_.post_after(cfg_.expire_interval_ns, [this] { tick(); });
            io_.run();
        });
    }
    ~EnrolService() {
        io_.stop();
        if (worker_.joinable()) worker_.join();
        // Nothing runs now. Sessions first (so a late completion's notify is cleared), then the sockets.
        for (auto& [serial, c] : conns_) {
            c->session.reset();
            io_.del_fd(c->fd);
            pal::close_fd(c->fd);
        }
        conns_.clear();
        for (auto& a : inbox_) pal::close_fd(a.fd);
        inbox_.clear();
    }
    EnrolService(const EnrolService&) = delete;
    EnrolService& operator=(const EnrolService&) = delete;

    // Called by the transport on its I/O thread. True: this service now owns `fd` (and will close it). False: nothing was taken; the caller closes it.
    [[nodiscard]] bool handoff(pal::fd_t fd, std::uint64_t peer_addr, std::vector<std::byte> initial) {
        if (!enr_.has_live_ticket(now_())) {
            refused_no_ticket_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        {
            std::lock_guard<std::mutex> g(mu_);
            auto& per_ip = per_ip_[peer_addr];
            if (active_ >= cfg_.max_concurrent || per_ip >= cfg_.max_per_ip) {
                if (per_ip == 0) per_ip_.erase(peer_addr);
                refused_caps_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            ++active_;
            ++per_ip;
            inbox_.push_back(Adopt{fd, peer_addr, std::move(initial)});
        }
        io_.post([this] { adopt_all(); });
        return true;
    }

    [[nodiscard]] std::size_t active() const {
        std::lock_guard<std::mutex> g(mu_);
        return active_;
    }
    [[nodiscard]] std::uint64_t refused_no_ticket() const noexcept { return refused_no_ticket_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t refused_caps() const noexcept { return refused_caps_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t replied() const noexcept { return replied_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t failed() const noexcept { return failed_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t timed_out() const noexcept { return timed_out_.load(std::memory_order_relaxed); }

private:
    struct Adopt {
        pal::fd_t fd;
        std::uint64_t addr;
        std::vector<std::byte> initial;
    };
    struct Conn {
        std::uint64_t serial = 0;
        pal::fd_t fd = pal::invalid_fd;
        std::uint64_t addr = 0;
        std::unique_ptr<EnrolAnchorSession> session;
        std::vector<std::byte> out;
        std::size_t out_off = 0;
        bool closing = false;      // the reply is sealed: close once it is flushed
        bool want_write = false;
    };

    static std::string source_of(std::uint64_t a) {
        return std::to_string((a >> 24) & 0xFF) + "." + std::to_string((a >> 16) & 0xFF) + "." + std::to_string((a >> 8) & 0xFF) + "." + std::to_string(a & 0xFF);
    }

    // Worker thread -------------------------------------------------------------------------------------------------
    void adopt_all() {
        std::vector<Adopt> batch;
        {
            std::lock_guard<std::mutex> g(mu_);
            batch.swap(inbox_);
        }
        for (auto& a : batch) adopt(std::move(a));
    }

    void adopt(Adopt a) {
        auto c = std::make_unique<Conn>();
        c->serial = ++serial_;
        c->fd = a.fd;
        c->addr = a.addr;
        const std::uint64_t serial = c->serial;
        c->session = std::make_unique<EnrolAnchorSession>(own_, enr_, source_of(a.addr), now_, [this, serial] { io_.post([this, serial] { on_session_ready(serial); }); });
        Conn* raw = c.get();
        conns_[serial] = std::move(c);
        if (!io_.add_fd(raw->fd, EPOLLIN, [this, serial](std::uint32_t ev) { on_event(serial, ev); })) {
            finish(serial, &failed_);
            return;
        }
        io_.post_after(cfg_.deadline_ns, [this, serial] {
            if (conns_.count(serial) != 0) finish(serial, &timed_out_);
        });
        if (!a.initial.empty()) process(*raw, a.initial);
    }

    void on_event(std::uint64_t serial, std::uint32_t ev) {
        const auto it = conns_.find(serial);
        if (it == conns_.end()) return;
        Conn& c = *it->second;
        if (ev & (EPOLLERR | EPOLLHUP)) return finish(serial, &failed_);
        if (ev & EPOLLOUT) {
            if (!flush(c)) return finish(serial, &failed_);
            if (c.closing && c.out_off >= c.out.size()) return finish(serial, &replied_);
        }
        if (ev & EPOLLIN) {
            std::array<std::byte, 16 * 1024> buf;
            for (int rounds = 0; rounds < 8; ++rounds) {  // bounded per wake-up: a fast sender cannot monopolise the worker
                auto r = pal::recv_some(c.fd, buf.data(), buf.size());
                if (!r) {
                    if (r.error() == pal::would_block()) return;
                    return finish(serial, &failed_);
                }
                if (*r == 0) return finish(serial, c.closing ? &replied_ : &failed_);
                process(c, std::span<const std::byte>(buf.data(), *r));
                if (conns_.count(serial) == 0) return;  // process() finished it
            }
        }
    }

    void process(Conn& c, std::span<const std::byte> in) {
        const std::uint64_t serial = c.serial;
        const auto st = c.session->feed(in, c.out);
        settle(c, st, serial);
    }

    void on_session_ready(std::uint64_t serial) {
        const auto it = conns_.find(serial);
        if (it == conns_.end()) return;
        Conn& c = *it->second;
        const auto st = c.session->drain(c.out);
        settle(c, st, serial);
    }

    void settle(Conn& c, EnrolAnchorSession::State st, std::uint64_t serial) {
        if (st == EnrolAnchorSession::State::Failed) {
            (void)flush(c);  // a TLS alert the session produced, best effort
            return finish(serial, &failed_);
        }
        if (st == EnrolAnchorSession::State::Replied) c.closing = true;
        if (!flush(c)) return finish(serial, &failed_);
        if (c.closing && c.out_off >= c.out.size()) finish(serial, &replied_);
    }

    // True unless the socket failed. Sets/clears write interest.
    bool flush(Conn& c) {
        while (c.out_off < c.out.size()) {
            auto r = pal::send_some(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off);
            if (!r) {
                if (r.error() == pal::would_block()) {
                    if (!c.want_write) {
                        c.want_write = true;
                        (void)io_.mod_fd(c.fd, EPOLLIN | EPOLLOUT);
                    }
                    return true;
                }
                return false;
            }
            c.out_off += *r;
        }
        c.out.clear();
        c.out_off = 0;
        if (c.want_write) {
            c.want_write = false;
            (void)io_.mod_fd(c.fd, EPOLLIN);
        }
        return true;
    }

    void finish(std::uint64_t serial, std::atomic<std::uint64_t>* counter) {
        const auto it = conns_.find(serial);
        if (it == conns_.end()) return;
        std::unique_ptr<Conn> c = std::move(it->second);
        conns_.erase(it);
        io_.del_fd(c->fd);
        pal::close_fd(c->fd);
        c->session.reset();
        counter->fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> g(mu_);
        --active_;
        const auto pit = per_ip_.find(c->addr);
        if (pit != per_ip_.end() && --pit->second == 0) per_ip_.erase(pit);
    }

    void tick() {
        (void)enr_.expire(now_());
        io_.post_after(cfg_.expire_interval_ns, [this] { tick(); });
    }

    TlsIdentity own_;
    enrol::Enrollment& enr_;
    EnrolServiceConfig cfg_;
    Clock now_;
    pal::IoContext io_;
    std::thread worker_;

    mutable std::mutex mu_;  // guards active_, per_ip_, inbox_ (shared between the handoff caller and the worker)
    std::size_t active_ = 0;
    std::unordered_map<std::uint64_t, std::size_t> per_ip_;
    std::vector<Adopt> inbox_;

    std::unordered_map<std::uint64_t, std::unique_ptr<Conn>> conns_;  // worker only
    std::uint64_t serial_ = 0;                                         // worker only

    std::atomic<std::uint64_t> refused_no_ticket_{0}, refused_caps_{0}, replied_{0}, failed_{0}, timed_out_{0};
};

}  // namespace quark::adapters
