// Shared fixture for the ADR-071 / ADR-068..071 red-team tests (#47): a one-hop TCP proxy with a
// per-connection kill switch. black_hole_existing() makes every connection that exists NOW silently
// swallow its bytes (no FIN, no RST: a Wi-Fi radio that vanished) while later connections work.
// Not a *_test.cpp: just an include. Same proxy as tests/mobile_member_network_changed_test.cpp.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "quark/net/tcp_transport.hpp"

namespace quark::test {

class BlackHoleProxy {
public:
    explicit BlackHoleProxy(std::uint16_t upstream_port) : upstream_port_(upstream_port) {}
    ~BlackHoleProxy() { stop(); }

    bool start() {
        auto l = pal::tcp_listen(pal::ipv4_loopback, 0);
        if (!l) return false;
        listen_fd_ = *l;
        auto p = pal::local_port(listen_fd_);
        if (!p) return false;
        port_ = *p;
        run_.store(true);
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        if (run_.exchange(false) && th_.joinable()) th_.join();
        for (auto& p : pipes_) {
            pal::close_fd(p.client);
            pal::close_fd(p.upstream);
        }
        pipes_.clear();
        if (listen_fd_ != pal::invalid_fd) pal::close_fd(listen_fd_);
        listen_fd_ = pal::invalid_fd;
    }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    // The old path dies silently: every connection that exists NOW stops forwarding (and discards).
    void black_hole_existing() { cut_generation_.fetch_add(1); }
    [[nodiscard]] std::uint64_t accepted() const noexcept { return accepted_.load(); }
    [[nodiscard]] std::uint64_t discarded_bytes() const noexcept { return discarded_.load(); }

private:
    struct Pipe {
        pal::fd_t client = pal::invalid_fd;
        pal::fd_t upstream = pal::invalid_fd;
        std::uint64_t born_generation = 0;
        std::vector<std::byte> to_up, to_client;  // bytes read but not yet written
    };

    static void pump(pal::fd_t from, pal::fd_t to, std::vector<std::byte>& pending, bool discard,
                     std::atomic<std::uint64_t>& discarded) {
        std::byte buf[4096];
        if (pending.empty()) {
            auto r = pal::recv_some(from, buf, sizeof buf);
            if (r && *r > 0) {
                if (discard) discarded.fetch_add(*r);
                else pending.assign(buf, buf + *r);
            }
        }
        while (!pending.empty()) {
            auto w = pal::send_some(to, pending.data(), pending.size());
            if (!w || *w == 0) break;
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(*w));
        }
    }

    void loop() {
        while (run_.load()) {
            for (int i = 0; i < 8; ++i) {  // accept whatever is waiting
                auto a = pal::accept_one(listen_fd_);
                if (!a) break;
                const pal::fd_t cfd = *a;
                auto u = pal::tcp_connect(pal::ipv4_loopback, upstream_port_);
                if (!u) {
                    pal::close_fd(cfd);
                    continue;
                }
                for (int t = 0; t < 200 && !pal::connect_result(*u); ++t)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                Pipe p;
                p.client = cfd;
                p.upstream = *u;
                p.born_generation = cut_generation_.load();
                pipes_.push_back(std::move(p));
                accepted_.fetch_add(1);
            }
            const std::uint64_t gen = cut_generation_.load();
            for (auto& p : pipes_) {
                const bool dead = gen > p.born_generation;
                pump(p.client, p.upstream, p.to_up, dead, discarded_);
                pump(p.upstream, p.client, p.to_client, dead, discarded_);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::uint16_t upstream_port_;
    pal::fd_t listen_fd_ = pal::invalid_fd;
    std::uint16_t port_ = 0;
    std::atomic<bool> run_{false};
    std::atomic<std::uint64_t> cut_generation_{0};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> discarded_{0};
    std::thread th_;
    std::vector<Pipe> pipes_;
};

}  // namespace quark::test
