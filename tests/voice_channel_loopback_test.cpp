// Tests the 028 VoiceChannel over real 127.0.0.1 UDP sockets: two "player" nodes exchange voice
// datagrams through a "relay" node they resolve via a capability-placed route (Flag{"voice-relay"}),
// exercising the real send -> AEAD seal -> udp_send_to -> udp_recv_from -> AEAD open -> relay fan-out
// path end to end. Also exercises replay/stale rejection (028 S2r) via the test-only feed hook, which
// bypasses the socket to get a deterministic double-delivery without racing real UDP timing.
#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "pal/net.hpp"
#include "quark/core/aead.hpp"
#include "quark/core/capabilities.hpp"
#include "quark/net/voice_channel.hpp"
#include "tcp_test_util.hpp"  // wait_until

using namespace quark;
using namespace quark::net;
using namespace quark::test;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// A loop-owning node: a real pal::IoContext driven by its own thread, exactly like TcpTransport's I/O
// thread. VoiceChannel is written against a *shared* pal::IoContext& (028's whole reuse point); a
// standalone test not running inside a TcpTransport has to supply and drive one itself.
struct LoopNode {
    pal::IoContext io;
    std::thread thread{[this] { io.run(); }};
    ~LoopNode() {
        io.stop();
        thread.join();
    }
};
}  // namespace

int main() {
    bool ok = true;
    const MockCipher cipher(0xC0FFEE'C0FFEEULL);

    // ---- End-to-end relay path over real loopback UDP sockets -----------------------------------
    LoopNode relay_loop, a_loop, b_loop;
    const NodeId R{3}, A{1}, B{2};

    VoiceChannel vr(relay_loop.io, R, cipher);
    VoiceChannel va(a_loop.io, A, cipher);
    VoiceChannel vb(b_loop.io, B, cipher);

    check(vr.bind(pal::ipv4_loopback, 0), "relay binds", ok);
    check(va.bind(pal::ipv4_loopback, 0), "player A binds", ok);
    check(vb.bind(pal::ipv4_loopback, 0), "player B binds", ok);
    if (!ok) {
        std::printf("voice_channel_loopback_test: FAIL (bind)\n");
        return 1;
    }

    // 025 capability placement: only R advertises Flag{"voice-relay"}, so every RoomId's route
    // resolves to R (a single-eligible-relay set is unambiguous) — the same VirtualBins machinery 026
    // uses for actor placement, restricted here to the voice-relay-eligible subset (028/ADR-030 C1).
    auto view = make_capability_view({A, B, R}, /*epoch=*/1,
                                      {{R, NodeCapabilities{Flag{"voice-relay"}}}});
    va.on_capability_view_changed(view);
    vb.on_capability_view_changed(view);

    // 021 discovery would normally hand back R's resolved Endpoint; wire it directly here (the same
    // shortcut tcp_transport_loopback_test.cpp takes with add_peer()). bind() already blocked until
    // the fd was actually registered, so fd()/local_port() are valid immediately after it returns.
    std::uint16_t relay_port = 0;
    {
        auto lp = pal::local_port(vr.fd());
        check(lp.has_value(), "relay local_port() resolves", ok);
        if (lp) relay_port = *lp;
    }
    va.ensure_peer_session(R, VoicePeer{R, pal::ipv4_loopback, relay_port});
    vb.ensure_peer_session(R, VoicePeer{R, pal::ipv4_loopback, relay_port});

    const RoomId room{42};
    std::mutex inbox_mu;
    std::vector<std::pair<NodeId, std::vector<std::byte>>> a_inbox, b_inbox;
    va.on_datagram([&](RoomId, NodeId from, std::span<const std::byte> payload) {
        std::lock_guard<std::mutex> g(inbox_mu);
        a_inbox.emplace_back(from, std::vector<std::byte>(payload.begin(), payload.end()));
    });
    vb.on_datagram([&](RoomId, NodeId from, std::span<const std::byte> payload) {
        std::lock_guard<std::mutex> g(inbox_mu);
        b_inbox.emplace_back(from, std::vector<std::byte>(payload.begin(), payload.end()));
    });

    const std::array<std::byte, 4> a_payload{std::byte{0xA1}, std::byte{0xA2}, std::byte{0xA3}, std::byte{0xA4}};
    const std::array<std::byte, 4> b_payload{std::byte{0xB1}, std::byte{0xB2}, std::byte{0xB3}, std::byte{0xB4}};

    // A speaks first: R learns A (room has only {A} so far, nothing to relay yet).
    va.send(room, a_payload);
    check(wait_until([&] { return vr.received() >= 1; }), "relay received A's frame", ok);

    // B speaks: R learns B, room now has {A,B}, relays B's frame to A (but never back to B itself).
    vb.send(room, b_payload);
    check(wait_until([&] { return vr.received() >= 2; }), "relay received B's frame", ok);
    check(wait_until([&] {
              std::lock_guard<std::mutex> g(inbox_mu);
              return !a_inbox.empty();
          }),
          "A received a relayed frame", ok);

    {
        std::lock_guard<std::mutex> g(inbox_mu);
        check(!a_inbox.empty() && a_inbox.front().first == B, "relayed frame is FROM B", ok);
        check(!a_inbox.empty() && a_inbox.front().second.size() == b_payload.size() &&
                  std::equal(a_inbox.front().second.begin(), a_inbox.front().second.end(), b_payload.begin()),
              "relayed payload intact", ok);
        check(b_inbox.empty(), "B (the sender) is never relayed its own frame", ok);
    }

    // Oversize payload is dropped locally, never touches the socket (028 delivery contract).
    {
        const std::vector<std::byte> big(kVoiceMaxPayload + 1);
        const std::uint64_t before = va.drop_oversize();
        va.send(room, big);
        check(wait_until([&] { return va.drop_oversize() > before; }), "oversize payload dropped locally", ok);
    }

    // ---- Replay/stale rejection (028 S2r), via the deterministic test-feed hook -----------------
    // A relay-side session is created from the first frame it ever sees from a peer; feed the SAME
    // sealed frame twice and confirm the second copy is rejected as stale/replayed, not double-counted.
    {
        LoopNode solo_loop;
        VoiceChannel solo(solo_loop.io, NodeId{9}, cipher);
        const NodeId sender{7};
        const std::array<std::byte, 3> payload{std::byte{1}, std::byte{2}, std::byte{3}};
        const auto sealed = solo.test_seal_frame(RoomId{1}, sender, /*seq=*/1, payload);

        solo.test_feed_datagram(sealed, pal::ipv4_loopback, 5555);
        check(wait_until([&] { return solo.received() >= 1; }), "first frame accepted", ok);

        solo.test_feed_datagram(sealed, pal::ipv4_loopback, 5555);  // exact replay
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(solo.received() == 1, "replayed frame NOT counted as received", ok);
        check(solo.drop_stale() >= 1, "replayed frame counted as a stale/replay drop", ok);
    }

    // ---- Sanity: loop-thread discipline held throughout (028 S3r) --------------------------------
    check(vr.thread_violations() == 0, "relay: no loop-thread violations", ok);
    check(va.thread_violations() == 0, "player A: no loop-thread violations", ok);
    check(vb.thread_violations() == 0, "player B: no loop-thread violations", ok);

    std::printf("voice_channel_loopback_test: %s  (relay recv=%llu sent=%llu, A inbox=%zu)\n",
                ok ? "OK" : "FAIL", static_cast<unsigned long long>(vr.received()),
                static_cast<unsigned long long>(vr.sent()), a_inbox.size());
    return ok ? 0 : 1;
}
