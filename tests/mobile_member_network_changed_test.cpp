// Tests ADR-071 claim C7 (a network switch ends in a clean reconnect: no duplicate frame, none lost
// after the change, the SecureTransport replay guard intact) over REAL sockets, per 019 §"Android target"
// (lifecycle contract) and 021 §"Mobile member profile". Implements MobileMember::network_changed().
//
// A PHONE node reaches a PC node only through a tiny TCP proxy. A "network switch" is modelled the way a
// phone sees it: the old path goes BLACK (the proxy keeps the old connection open and silently discards
// everything on it, no FIN/RST, exactly a Wi-Fi radio that vanished) while a NEW connection through the
// proxy works (cellular). Nothing in the phone tells it the old socket is dead.
//
// Phases and what each proves:
//   P1  50 frames flow normally.                                       (baseline; replay guard idle)
//   P2  the old path goes black; 10 frames are sent into it.            (in flight at the failure: gone)
//   P3  CONTROL: NO network_changed(); 10 more frames are sent and wait. They are NOT delivered, and the
//       phone's transport still believes it has one healthy connection with zero reconnects. This is the
//       existing behaviour without the mobile handling (TCP would only notice after its own timeouts).
//   P4  MobileMember::network_changed(), then 50 frames sent IMMEDIATELY (queued while the new connection
//       is still being dialled). All 50 arrive exactly once, in order; receiver replays_rejected() == 0;
//       the phone reconnected; one connection is open.
// Final assertion: the PC received exactly [0..49] ++ [70..119]. Frames 50..69 were handed to a dead path
// (at-most-once per 010; the 017 delivery layer above is what retries) and appear NOWHERE twice.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "quark/core/aead.hpp"
#include "quark/core/mobile_member.hpp"
#include "quark/core/secure_transport.hpp"
#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

// One-hop TCP proxy with a per-connection kill switch (silent black hole).
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

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // progress is visible even if a wait stalls
    const NodeId PC{1}, PHONE{2};
    MockCipher cipher(0x71);  // static-key mode: ONE session per peer for the whole run, so the replay
                              // guard's per-lane sequence numbers must keep rising across the reconnect.

    net::TcpTransport tcp_pc(PC, pal::ipv4_loopback, 0);
    net::TcpTransport tcp_phone(PHONE, pal::ipv4_loopback, 0);
    SecureTransport sec_pc(tcp_pc, cipher, PC);
    SecureTransport sec_phone(tcp_phone, cipher, PHONE);
    Recorder at_pc;
    sec_pc.on_receive([&](MessageFrame f) { at_pc(std::move(f)); });
    sec_phone.on_receive([](MessageFrame) {});
    tcp_phone.set_peer_down_hook([&](NodeId p) { sec_phone.on_peer_disconnected(p); });
    tcp_pc.set_peer_down_hook([&](NodeId p) { sec_pc.on_peer_disconnected(p); });
    if (!(tcp_pc.start() && tcp_phone.start())) {
        std::fprintf(stderr, "startup failed\n");
        return 1;
    }
    BlackHoleProxy proxy(tcp_pc.listen_port());
    if (!proxy.start()) {
        std::fprintf(stderr, "proxy failed\n");
        return 1;
    }
    tcp_phone.add_peer(loopback_endpoint(PC, proxy.port()));  // the phone reaches the PC only via the proxy

    // The membership side of the MobileMember is exercised by the other suites; here the node has no peers
    // in a SWIM view, so the reset callback is driven for the one known peer explicitly.
    LoopbackFabric fabric;
    LoopbackTransport lt(fabric, PHONE);
    SwimMembership::Config cfg;
    cfg.cluster_id = ClusterId{0x7C};
    SwimMembership swim(PHONE, lt, cfg);
    (void)swim.admit(PC, cfg.cluster_id);  // so view() lists the PC and network_changed() resets its connection
    MobileMember mobile(swim, [&](NodeId p) { tcp_phone.reset_peer_connection(p); });

    auto send_range = [&](std::uint64_t lo, std::uint64_t hi) {
        for (std::uint64_t s = lo; s < hi; ++s) sec_phone.send(PC, seq_frame(PHONE, PC, s));
    };

    // P1
    send_range(0, 50);
    check(wait_until([&] { return at_pc.count() >= 50; }, 3000), "P1: 50 frames delivered over the first path");
    check(sec_pc.replays_rejected() == 0, "P1: no replay rejected");
    check(proxy.accepted() == 1, "P1: exactly one connection through the proxy");

    // P2: the first path goes black. The phone is not told.
    proxy.black_hole_existing();
    send_range(50, 60);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(at_pc.count() == 50, "P2: frames sent into the black hole are not delivered");

    // P3: CONTROL. No network_changed(): still nothing, and the transport thinks all is well.
    send_range(60, 70);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    check(at_pc.count() == 50, "P3 control: without network_changed() nothing is delivered");
    check(tcp_phone.reconnects() == 0, "P3 control: the transport has not noticed (0 reconnects)");
    check(tcp_phone.connections_open() == 1, "P3 control: it still counts one healthy connection");
    std::printf("  P3 control (no network_changed): delivered=%llu of 70 sent, reconnects=%llu, proxy discarded %llu bytes\n",
                static_cast<unsigned long long>(at_pc.count()), static_cast<unsigned long long>(tcp_phone.reconnects()),
                static_cast<unsigned long long>(proxy.discarded_bytes()));

    // P4: the app reports the network change; frames are sent at once (queued during the redial).
    mobile.network_changed();
    send_range(70, 120);
    check(wait_until([&] { return at_pc.count() >= 100; }, 3000), "P4: the 50 post-change frames all arrive");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // would expose a late duplicate
    check(at_pc.count() == 100, "P4: exactly 100 frames in total, no duplicate");
    check(tcp_phone.reconnects() >= 1, "P4: the phone reconnected");
    check(wait_until([&] { return tcp_phone.connections_open() == 1; }, 3000), "P4: one connection open");
    check(proxy.accepted() == 2, "P4: exactly one NEW connection through the proxy");
    check(sec_pc.replays_rejected() == 0, "P4: replay guard rejected nothing (sequence stayed strictly rising)");
    check(mobile.network_changes() == 1 && swim.network_changes() == 1, "network_changed() counted once");

    {
        std::lock_guard<std::mutex> g(at_pc.m);
        std::vector<std::uint64_t> expect;
        for (std::uint64_t s = 0; s < 50; ++s) expect.push_back(s);
        for (std::uint64_t s = 70; s < 120; ++s) expect.push_back(s);
        check(at_pc.seqs == expect, "PC received exactly [0..49] ++ [70..119], in order, each once");
    }
    std::printf("  P4: delivered=%llu (50 before + 50 after), reconnects=%llu, replays_rejected=%llu, connections via proxy=%llu\n",
                static_cast<unsigned long long>(at_pc.count()), static_cast<unsigned long long>(tcp_phone.reconnects()),
                static_cast<unsigned long long>(sec_pc.replays_rejected()),
                static_cast<unsigned long long>(proxy.accepted()));

    proxy.stop();
    tcp_phone.stop();
    tcp_pc.stop();
    std::printf(g_ok ? "mobile_member_network_changed_test: OK\n" : "mobile_member_network_changed_test: FAILED\n");
    return g_ok ? 0 : 1;
}
