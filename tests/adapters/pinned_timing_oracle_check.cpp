// Issue #69 measured, issue #56 / ADR-083 fixes: does the pinned responder's close time reveal whether a Msg1 was built for
// its right static key? With --gate against the LOCAL responder: exit 1 if the sets are distinguishable (KS p <= 0.01).
// Against a remote responder (--host/--port) it only reports. Controls: --inject-delay-us (a 1 ms oracle must be detected)
// and --floor-off (the refusal floor disabled = the pre-fix behaviour, which must be detected as distinguishable).
//   set A  a REAL Msg1 built for the responder's static key, from a static key the responder has NOT admitted
//   set B  random bytes of the same length inside a well-formed Authenticate frame (a raw-random buffer would die in the
//          frame decoder, a different code path, and would make any difference meaningless)
// Each sample: connect, send the whole frame, then spin on recv until the peer closes; the time is steady_clock from the
// end of send to the observed close. A and B are interleaved (ABAB...) to cancel drift. Reported per set: median, p5, p95,
// and the two-sample Kolmogorov-Smirnov D and p-value.
//   CONTROL  --inject-delay-us 1000 sleeps 1 ms on every set-A sample's close path before recording; the KS test must then
//            give p < 0.01, otherwise the check exits 1 (the measurement cannot see a 1 ms oracle).
//   LAN      --host IP --port P --responder-pub HEX64 aims the same measurement at a remote pinned responder whose Ed25519
//            public key is given (the local responder is not started).
// Implements 020-Security §"Pinned-key mode (Noise IK, #40)" (silent failure, ADR-072); measures the #56 timing oracle.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sodium.h>

#include "pinned_tcp_util.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/net/tcp_transport.hpp"
#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {
const ClusterId kCluster = kPinnedTcpCluster;
using Clock = std::chrono::steady_clock;

struct Args {
    std::size_t n = 10000;
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::string responder_pub_hex;
    long inject_us = 0;
    bool floor_off = false;
    bool gate = false;
    long floor_us = -1;
};

bool parse_hex32(const std::string& s, Ed25519PublicKey& out) {
    if (s.size() != 2 * out.size()) return false;
    for (std::size_t i = 0; i < out.size(); ++i) {
        unsigned v = 0;
        const char* b = s.data() + 2 * i;
        const auto r = std::from_chars(b, b + 2, v, 16);
        if (r.ec != std::errc{} || r.ptr != b + 2) return false;
        out[i] = static_cast<std::byte>(v);
    }
    return true;
}

// Connect, send all of `wire`, then wait for the peer to close. Returns the nanoseconds from the end of send to the
// observed close (EOF or error), or -1 if the connect/send failed or the peer never closed within the deadline.
long long timed_close(const std::string& host, std::uint16_t port, const Bytes& wire, long inject_us) {
    unsigned o[4] = {127, 0, 0, 1};
    {
        const char* p = host.data();
        const char* const end = p + host.size();
        for (int i = 0; i < 4; ++i) {
            const auto r = std::from_chars(p, end, o[i]);
            if (r.ec != std::errc{} || o[i] > 255 || (i < 3 ? (r.ptr == end || *r.ptr != '.') : r.ptr != end)) return -1;
            p = r.ptr + (i < 3 ? 1 : 0);
        }
    }
    auto c = pal::tcp_connect(pal::ipv4(static_cast<std::uint8_t>(o[0]), static_cast<std::uint8_t>(o[1]),
                                        static_cast<std::uint8_t>(o[2]), static_cast<std::uint8_t>(o[3])), port);
    if (!c) return -1;
    const pal::fd_t fd = *c;
    std::size_t sent = 0;
    const auto give_up = Clock::now() + std::chrono::seconds(3);
    while (sent < wire.size()) {
        auto s = pal::send_some(fd, wire.data() + sent, wire.size() - sent);
        if (s) sent += *s;
        else if (Clock::now() > give_up) { pal::close_fd(fd); return -1; }
    }
    const auto t0 = Clock::now();
    bool closed = false;
    while (!closed && Clock::now() < give_up) {
        std::byte buf[512];
        auto r = pal::recv_some(fd, buf, sizeof(buf));
        if (r && *r == 0) closed = true;
        else if (!r && r.error() != pal::would_block()) closed = true;
        else if (r) { pal::close_fd(fd); return -1; }  // bytes back: the responder is not silent, not what we measure
    }
    if (closed && inject_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(inject_us));
    const auto t1 = Clock::now();
    pal::close_fd(fd);
    return closed ? std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() : -1;
}

double quantile(const std::vector<long long>& sorted, double q) {
    if (sorted.empty()) return 0;
    const double idx = q * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(idx);
    const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
    const double fr = idx - static_cast<double>(lo);
    return static_cast<double>(sorted[lo]) * (1 - fr) + static_cast<double>(sorted[hi]) * fr;
}

// Two-sample Kolmogorov-Smirnov: D and the asymptotic p-value. Inputs sorted.
void ks(const std::vector<long long>& a, const std::vector<long long>& b, double& D, double& p) {
    D = 0;
    std::size_t i = 0, j = 0;
    const double n = static_cast<double>(a.size()), m = static_cast<double>(b.size());
    while (i < a.size() && j < b.size()) {
        const long long v = std::min(a[i], b[j]);
        while (i < a.size() && a[i] == v) ++i;
        while (j < b.size() && b[j] == v) ++j;
        D = std::max(D, std::fabs(static_cast<double>(i) / n - static_cast<double>(j) / m));
    }
    const double ne = n * m / (n + m);
    const double lambda = (std::sqrt(ne) + 0.12 + 0.11 / std::sqrt(ne)) * D;
    double sum = 0;
    for (int k = 1; k <= 100; ++k) {
        const double term = std::exp(-2.0 * k * k * lambda * lambda);
        sum += (k % 2 == 1 ? 1.0 : -1.0) * term;
        if (term < 1e-12) break;
    }
    p = std::clamp(2.0 * sum, 0.0, 1.0);
}

struct Stats {
    std::vector<long long> v;
    void print(const char* name) const {
        std::printf("  %-4s n=%-6zu median=%10.0f ns   p5=%10.0f ns   p95=%10.0f ns\n", name, v.size(), quantile(v, 0.5),
                    quantile(v, 0.05), quantile(v, 0.95));
    }
};

Bytes frame_of(const Bytes& payload) {
    MessageFrame f{};
    f.kind = FrameKind::Authenticate;
    f.payload = payload;
    return net::encode_frame(f);
}

// One full measurement. Returns false if the setup failed. p_out is the KS p-value.
bool measure(const Args& args, Node& attacker, NodeId responder_id, std::uint16_t port, double& p_out, bool print) {
    std::vector<long long> a, b;
    a.reserve(args.n);
    b.reserve(args.n);
    std::size_t failed = 0;
    Bytes msg1;
    for (std::size_t i = 0; i < args.n; ++i) {
        auto eng = attacker.engine(true, responder_id, kCluster);
        Bytes m;
        if (eng->advance({}, m) != HandshakeEngine::Step::WantWrite) {
            std::fprintf(stderr, "  setup: the initiator engine produced no Msg1\n");
            return false;
        }
        const Bytes wire_a = frame_of(m);
        Bytes rnd(m.size());
        randombytes_buf(rnd.data(), rnd.size());
        const Bytes wire_b = frame_of(rnd);
        const long long ta = timed_close(args.host, port, wire_a, args.inject_us);  // A, then B: interleaved
        const long long tb = timed_close(args.host, port, wire_b, 0);
        if (ta < 0 || tb < 0) { ++failed; continue; }
        a.push_back(ta);
        b.push_back(tb);
    }
    if (a.size() < 100) {
        std::fprintf(stderr, "  setup: only %zu usable samples (%zu failed pairs)\n", a.size(), failed);
        return false;
    }
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    double D = 0;
    ks(a, b, D, p_out);
    if (print) {
        Stats{a}.print("A");
        Stats{b}.print("B");
        std::printf("  KS   D=%.4f   p=%.6g   (%zu pairs, %zu failed pairs dropped)\n", D, p_out, a.size(), failed);
    }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (k == "--host") args.host = next();
        else if (k == "--port") args.port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else if (k == "--n") args.n = static_cast<std::size_t>(std::atoll(next().c_str()));
        else if (k == "--responder-pub") args.responder_pub_hex = next();
        else if (k == "--floor-off") args.floor_off = true;
        else if (k == "--gate") args.gate = true;
        else if (k == "--floor-us") args.floor_us = std::atol(next().c_str());
        else if (k == "--inject-delay-us") args.inject_us = std::atol(next().c_str());
        else { std::fprintf(stderr, "unknown argument %s\n", k.c_str()); return 2; }
    }
    if (!adapters::noise::init()) return 1;

    // The attacker: a static key the responder has not admitted; it knows the responder's key (so its Msg1 is "correct").
    std::vector<std::unique_ptr<Node>> pool;
    std::unique_ptr<Node> local_responder;
    std::unique_ptr<Stack> stack;
    Ed25519PublicKey resp_pub{};
    NodeId resp_id{};
    std::uint16_t port = args.port;
    if (args.port == 0) {
        for (int i = 0; i < 2; ++i) pool.push_back(std::make_unique<Node>());
        std::sort(pool.begin(), pool.end(), [](auto& x, auto& y) { return x->id.value < y->id.value; });
        local_responder = std::move(pool[1]);
        resp_pub = local_responder->pub;
        resp_id = local_responder->id;
        stack = std::make_unique<Stack>(*local_responder, true);
        if (args.floor_off) stack->tcp.set_anonymous_refusal_floor_ns(0);
        else if (args.floor_us >= 0) stack->tcp.set_anonymous_refusal_floor_ns(static_cast<std::uint64_t>(args.floor_us) * 1000u);
        if (!stack->start()) { std::fprintf(stderr, "responder failed to start\n"); return 1; }
        port = stack->tcp.listen_port();
    } else {
        if (!parse_hex32(args.responder_pub_hex, resp_pub)) {
            std::fprintf(stderr, "--port needs --responder-pub <64 hex digits>\n");
            return 2;
        }
        resp_id = derive_pinned_node_id(resp_pub, PinnedRole::Member, NodeIdDerivation::RoleBit);
    }
    std::unique_ptr<Node> attacker;
    do attacker = std::make_unique<Node>();
    while (attacker->id.value >= resp_id.value);  // the lower id dials
    (void)attacker->book->admit(kCluster, resp_pub, PinnedRole::Member);  // it knows the responder; the responder does not know it

    std::printf("pinned_timing_oracle_check: Msg1 for the right key (A) vs random bytes (B), host=%s port=%u n=%zu%s\n",
                args.host.c_str(), static_cast<unsigned>(port), args.n,
                args.inject_us ? "  [CONTROL: delay injected on A]" : "");
    double p = 1;
    if (!measure(args, *attacker, resp_id, port, p, true)) return 1;

    if (args.inject_us > 0) {
        if (p < 0.01) std::printf("CONTROL detected (p < 0.01): the measurement sees a %ld us oracle\n", args.inject_us);
        else {
            std::fprintf(stderr, "CONTROL NOT detected (p=%g >= 0.01): the measurement cannot see a %ld us oracle\n", p,
                         args.inject_us);
            return 1;
        }
        return 0;
    }
    std::printf("%s (p %s 0.01)\n", p <= 0.01 ? "DISTINGUISHABLE" : "not distinguishable", p <= 0.01 ? "<=" : ">");
    if (args.port != 0) return 0;  // remote: report only
    std::printf("  floor overruns: %llu\n", static_cast<unsigned long long>(stack->tcp.anonymous_refusal_floor_overruns()));
    if (args.floor_off) {  // CONTROL: the pre-fix behaviour must be seen
        if (p <= 0.01) { std::printf("CONTROL (floor off) detected: the oracle exists without the floor\n"); return 0; }
        std::fprintf(stderr, "CONTROL (floor off) NOT detected (p=%g): the measurement cannot see the original oracle\n", p);
        return 1;
    }
    return (args.gate && p <= 0.01) ? 1 : 0;  // --gate: the acceptance test of #56 (not yet met)
}
