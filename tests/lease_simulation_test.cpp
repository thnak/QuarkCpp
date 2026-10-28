// Tests 028-Leases-and-Leader-Election / ADR-058: a deterministic, seeded simulation of N candidates,
// one lease arbiter backed by a REAL InMemoryStore, and a lossy / delaying / duplicating /
// partitionable message fabric, with per-node clock offset + drift, node pauses (zombie leaders) and
// arbiter restarts. Safety invariants asserted in EVERY scenario (clock-independent):
//   S1 every term is issued to exactly one holder and terms are strictly monotonic (no reuse, no regress);
//   S2 the Store accepts a write iff its token >= the Store's owner token, and ANY write carrying a
//      token below the highest term ever granted is REJECTED (deposed leader fenced out);
//   S3 accepted writes carry non-decreasing tokens in real-time order (no old leader lands after a new one);
//   S4 the Store's log is exactly the accepted writes (contiguous seq, no torn/duplicate commit).
// In scenarios whose drift stays WITHIN the configured bound, additionally:
//   S5 at most one node BELIEVES it is leader at any real instant (lease-belief exclusivity).
// Scenarios with out-of-bound drift only assert S1-S4 (belief overlap is counted and reported).
// Liveness is best-effort and only asserted for the fault-free scenario class.
// Scenario count: env QUARK_LEASE_SCENARIOS (default 10000).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <queue>
#include <unordered_map>
#include <vector>

#include "quark/core/lease.hpp"

using namespace quark;
using namespace quark::lease;

namespace {

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t n) { return n ? next() % n : 0; }
    bool chance(std::uint32_t per_mille) { return below(1000) < per_mille; }
};

struct Msg {
    std::uint64_t at, seq;
    Peer to;
    Request rq;
    Reply rp;
};
struct MsgLater {
    bool operator()(const Msg& a, const Msg& b) const { return a.at != b.at ? a.at > b.at : a.seq > b.seq; }
};

struct Clock {
    std::uint64_t offset = 0;
    std::uint32_t rate_ppm = 1'000'000;
    [[nodiscard]] Ticks at(std::uint64_t real) const { return offset + real * rate_ppm / 1'000'000ull; }
};

struct Stats {
    std::uint64_t scenarios = 0, bounded = 0, writes_ok = 0, writes_rejected = 0, zombie_rejected = 0, terms = 0,
                  belief_overlap_bounded = 0, belief_overlap_unbounded_scen = 0, leader_at_end = 0, faultfree = 0,
                  faultfree_leader = 0, fails = 0;
};

#define SIM_CHECK(cond, ...)                                                      \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::printf("FAIL seed=%llu: %s  ", static_cast<unsigned long long>(seed), #cond); \
            std::printf(__VA_ARGS__);                                             \
            std::printf("\n");                                                    \
            ++st.fails;                                                           \
            return;                                                               \
        }                                                                         \
    } while (0)

void run_scenario(std::uint64_t seed, Stats& st) {
    Rng rng{seed * 0x2545F4914F6CDD1Dull + 12345};
    const bool faultfree = (seed % 8 == 0);
    const bool bounded = faultfree || (seed % 3 != 0);  // ~1/3 of faulty scenarios violate the drift bound
    const unsigned N = 3 + static_cast<unsigned>(rng.below(3));
    const Peer ARB = N;
    Config cfg;  // D=100, rho=5%
    const std::uint32_t loss = faultfree ? 0 : static_cast<std::uint32_t>(rng.below(450));
    const std::uint32_t dup = faultfree ? 0 : static_cast<std::uint32_t>(rng.below(150));
    const std::uint64_t maxdelay = faultfree ? 3 : 1 + rng.below(140);  // may exceed D
    const std::uint32_t cutp = static_cast<std::uint32_t>(rng.below(600));
    const std::uint64_t H = 1500;

    InMemoryStore store;
    const ActorId key{TypeKey{7}, 1};
    LeaseArbiter<InMemoryStore> arb(store, key, cfg);
    std::vector<LeaseHolder> nodes;
    std::vector<Clock> clk(N + 1);
    std::vector<std::uint64_t> pause_until(N, 0);
    for (Peer i = 0; i < N; ++i) nodes.emplace_back(i, cfg);
    for (auto& c : clk) {
        c.offset = rng.below(1'000'000'000'000ull);
        if (faultfree) {
            c.rate_ppm = 1'000'000 + static_cast<std::uint32_t>(rng.below(20'001)) - 10'000;
        } else if (bounded) {
            c.rate_ppm = 1'000'000 - cfg.drift_ppm_bound +
                         static_cast<std::uint32_t>(rng.below(2ull * cfg.drift_ppm_bound + 1));
        } else {  // 0.4x .. 2.5x: far outside the 5% assumption
            c.rate_ppm = 400'000 + static_cast<std::uint32_t>(rng.below(2'100'000));
        }
    }
    std::vector<std::vector<bool>> cut(N + 1, std::vector<bool>(N + 1, false));

    std::priority_queue<Msg, std::vector<Msg>, MsgLater> q;
    std::uint64_t mseq = 0;
    auto send = [&](Peer from, Peer to, Request rq, Reply rp, std::uint64_t now) {
        if (cut[from][to] || rng.chance(loss)) return;
        const int copies = rng.chance(dup) ? 2 : 1;
        for (int c = 0; c < copies; ++c) q.push(Msg{now + 1 + rng.below(maxdelay), mseq++, to, rq, rp});
    };

    std::unordered_map<std::uint64_t, Peer> term_holder;
    std::uint64_t max_granted = 0, max_accepted = 0, seen_terms = 0, ok_here = 0;
    bool overlap_seen = false;

    for (std::uint64_t t = 0; t < H; ++t) {
        if (!faultfree) {
            if (t % 150 == 0)
                for (unsigned a = 0; a <= N; ++a)
                    for (unsigned b = 0; b <= N; ++b) cut[a][b] = (a != b) && rng.chance(cutp);
            if (rng.chance(4)) pause_until[rng.below(N)] = t + 1 + rng.below(300);  // long pause => zombie
            if (rng.chance(2)) arb.restart(clk[ARB].at(t));
        }
        while (!q.empty() && q.top().at <= t) {
            Msg m = q.top();
            q.pop();
            if (m.to == ARB) {
                if (auto rp = arb.on_request(clk[ARB].at(t), m.rq)) {
                    if (rp->granted) {  // S1, observed from the arbiter's grants
                        const auto v = rp->term.value;
                        auto it = term_holder.find(v);
                        if (it == term_holder.end()) {
                            SIM_CHECK(v > max_granted, "term %llu not above max %llu", static_cast<unsigned long long>(v),
                                      static_cast<unsigned long long>(max_granted));
                            term_holder[v] = rp->to;
                            max_granted = v;
                            ++seen_terms;
                        } else {
                            SIM_CHECK(it->second == rp->to, "term %llu issued to two holders", static_cast<unsigned long long>(v));
                        }
                    }
                    send(ARB, rp->to, {}, *rp, t);
                }
            } else {
                auto& n = nodes[m.to];
                n.on_reply(clk[m.to].at(t), m.rp);
                if (!m.rp.granted) n.backoff_until(clk[m.to].at(t) + rng.below(cfg.duration / 2));
            }
        }
        for (Peer i = 0; i < N; ++i) {
            if (t < pause_until[i]) continue;  // frozen (GC pause / SIGSTOP)
            if (auto rq = nodes[i].tick(clk[i].at(t))) send(i, ARB, *rq, {}, t);
        }
        // Writes: believed leaders write every tick; nodes holding only a STALE token (zombies) write
        // too, with no regard to belief (including paused ones: a resumed process acts before it ticks).
        for (Peer i = 0; i < N; ++i) {
            const FenceToken tok = nodes[i].token();
            if (is_unfenced(tok)) continue;
            if (!nodes[i].believes_leader(clk[i].at(t)) && !rng.chance(250)) continue;
            const std::byte payload[1] = {std::byte{static_cast<unsigned char>(i)}};
            const SeqNo seq = store.last_seq(key) + 1;
            const FenceToken owner = store.current_owner(key);
            const bool accepted = store.append(key, tok, seq, payload).has_value();
            SIM_CHECK(accepted == (tok.value >= owner.value), "store accept=%d tok=%llu owner=%llu", accepted,
                      static_cast<unsigned long long>(tok.value), static_cast<unsigned long long>(owner.value));
            if (tok.value < max_granted) {
                SIM_CHECK(!accepted, "deposed token %llu (< granted %llu) accepted", static_cast<unsigned long long>(tok.value),
                          static_cast<unsigned long long>(max_granted));
                ++st.zombie_rejected;
            }
            if (accepted) {
                SIM_CHECK(tok.value >= max_accepted, "S3: accepted %llu after %llu", static_cast<unsigned long long>(tok.value),
                          static_cast<unsigned long long>(max_accepted));
                max_accepted = tok.value;
                ++st.writes_ok;
                ++ok_here;
            } else {
                ++st.writes_rejected;
            }
        }
        unsigned believers = 0;
        for (Peer i = 0; i < N; ++i) believers += nodes[i].believes_leader(clk[i].at(t)) ? 1u : 0u;
        if (believers > 1) {
            overlap_seen = true;
            if (bounded) ++st.belief_overlap_bounded;
        }
    }
    SIM_CHECK(store.last_seq(key) == ok_here, "S4: log seq %llu != accepted writes %llu",
              static_cast<unsigned long long>(store.last_seq(key)), static_cast<unsigned long long>(ok_here));

    unsigned believers = 0;
    for (Peer i = 0; i < N; ++i) believers += nodes[i].believes_leader(clk[i].at(H)) ? 1u : 0u;
    if (believers) ++st.leader_at_end;
    if (faultfree) {
        ++st.faultfree;
        if (believers) ++st.faultfree_leader;
    }
    if (!bounded && overlap_seen) ++st.belief_overlap_unbounded_scen;
    if (bounded) ++st.bounded;
    st.terms += seen_terms;
    ++st.scenarios;
}

}  // namespace

int main() {
    std::uint64_t n = 10000;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // std::getenv: portable, std-only; single read before any threads
#endif
    if (const char* e = std::getenv("QUARK_LEASE_SCENARIOS")) n = std::strtoull(e, nullptr, 10);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    Stats st;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t seed = 1; seed <= n && st.fails == 0; ++seed) run_scenario(seed, st);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    std::printf("lease simulation: %llu scenarios (%llu drift-bounded) in %.2fs\n", u(st.scenarios), u(st.bounded), secs);
    std::printf("  terms issued %llu | store writes accepted %llu rejected %llu (of which stale-token %llu)\n",
                u(st.terms), u(st.writes_ok), u(st.writes_rejected), u(st.zombie_rejected));
    std::printf("  belief-overlap ticks in drift-bounded scenarios: %llu (must be 0)\n", u(st.belief_overlap_bounded));
    std::printf("  out-of-bound-drift scenarios with belief overlap: %llu (informational; Store fence still held)\n",
                u(st.belief_overlap_unbounded_scen));
    std::printf("  liveness (best-effort; asserted only fault-free): leader at end in %llu/%llu; fault-free %llu/%llu\n",
                u(st.leader_at_end), u(st.scenarios), u(st.faultfree_leader), u(st.faultfree));
    if (st.fails) return 1;
    if (st.belief_overlap_bounded != 0) { std::printf("FAIL: S5 belief exclusivity\n"); return 1; }
    if (st.faultfree_leader != st.faultfree) { std::printf("FAIL: fault-free liveness\n"); return 1; }
    if (st.zombie_rejected == 0 || st.terms == 0) { std::printf("FAIL: vacuous run\n"); return 1; }
    std::printf("PASS\n");
    return 0;
}
