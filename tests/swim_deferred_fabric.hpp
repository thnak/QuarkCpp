// Shared harness for the ADR-049 SWIM tests (NOT a *_test.cpp — just an include). 014 fault injection:
// a deterministic test transport that NEVER delivers in the sender's call stack. send() only queues;
// Sim::round() ticks every node, then flushes the queue wave by wave (frames produced while a wave is
// delivered go to the next wave). Optional seeded shuffle per wave, a per-directed-link policy
// (Deliver / Drop / Hold — Hold models TcpTransport buffering a cut link and releasing it on heal),
// and a per-wave virtual-clock advance `d` as a latency model.
//
// Why this exists: LoopbackTransport delivers inline, so a reply lands inside the sender's own call.
// That hid ADR-049's relay defect (the PingReq handler checked for the Ack before returning) and the
// partition-heal collapse (stale gossip racing the subject's refutation). Every timing-dependent SWIM
// test should run on this, not on the loopback.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quark/core/cluster.hpp"
#include "quark/core/transport.hpp"

namespace quark::test {

struct VClock {
    std::int64_t now = 0;
};
inline std::int64_t vclock_read(void* c) noexcept { return static_cast<VClock*>(c)->now; }

inline std::uint64_t sm64(std::uint64_t& s) {
    std::uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

enum class Link : std::uint8_t { Deliver, Drop, Hold };

class DeferredFabric {
public:
    using Sink = std::function<void(MessageFrame)>;
    void attach(NodeId n, Sink cb) { sinks_[n.value] = std::move(cb); }
    void send(NodeId from, NodeId to, MessageFrame f) { q_.push_back(Item{from, to, std::move(f)}); }

    // Deliver up to max_waves waves. d_ns > 0: the clock advances by d before each wave, and a wave
    // that would reach limit_ns (the next tick) is not run — its frames stay queued.
    void flush(int max_waves, std::uint64_t shuffle_seed, std::int64_t d_ns, std::int64_t limit_ns, VClock& clk) {
        for (int w = 0; w < max_waves && !q_.empty(); ++w) {
            if (d_ns > 0) {
                if (clk.now + d_ns >= limit_ns) break;
                clk.now += d_ns;
            }
            std::vector<Item> wave = std::exchange(q_, {});
            if (shuffle_seed) {
                std::uint64_t s = shuffle_seed * std::uint64_t{0x9E37} + static_cast<std::uint64_t>(w);
                for (std::size_t i = wave.size(); i > 1; --i) std::swap(wave[i - 1], wave[sm64(s) % i]);
            }
            for (Item& it : wave) {
                const Link l = link ? link(it.from, it.to) : Link::Deliver;
                if (l == Link::Drop) continue;
                if (l == Link::Hold) {
                    held_.push_back(std::move(it));
                    continue;
                }
                const auto s = sinks_.find(it.to.value);
                if (s == sinks_.end()) continue;
                Sink cb = s->second;  // copy: the callback may re-attach (a restarted node)
                cb(std::move(it.f));
            }
        }
    }
    void release_held() {
        for (Item& it : held_) q_.push_back(std::move(it));
        held_.clear();
    }

    std::function<Link(NodeId, NodeId)> link;

private:
    struct Item {
        NodeId from;
        NodeId to;
        MessageFrame f;
    };
    std::vector<Item> q_, held_;
    std::unordered_map<std::uint64_t, Sink> sinks_;
};

class DeferredTransport final : public Transport {
public:
    DeferredTransport(DeferredFabric& f, NodeId self) : f_(&f), self_(self) {}
    void send(NodeId to, MessageFrame frame) override { f_->send(self_, to, std::move(frame)); }
    void on_receive(std::function<void(MessageFrame)> cb) override { f_->attach(self_, std::move(cb)); }

private:
    DeferredFabric* f_;
    NodeId self_;
};

// N SWIM nodes on one fabric and one virtual clock. Every node is admitted to every other up front.
struct Sim {
    VClock clk;
    DeferredFabric fab;
    std::map<std::uint64_t, std::unique_ptr<DeferredTransport>> transports;
    std::map<std::uint64_t, std::unique_ptr<SwimMembership>> nodes;
    SwimMembership::Config cfg;
    std::int64_t tick_ns = 120'000'000;  // protocol period
    int max_waves = 8;
    std::uint64_t shuffle = 0;  // 0 = FIFO
    std::int64_t d = 0;         // per-wave latency
    std::int64_t tick_no = 0;

    SwimMembership& add(std::uint64_t id) {
        transports[id] = std::make_unique<DeferredTransport>(fab, NodeId{id});
        SwimMembership::Config c = cfg;
        c.seed = cfg.seed ^ (id * 0x9E3779B97F4A7C15ULL);
        nodes[id] = std::make_unique<SwimMembership>(NodeId{id}, *transports[id], c);
        nodes[id]->set_clock(&vclock_read, &clk);
        return *nodes[id];
    }
    void admit_all() {
        for (auto& [a, na] : nodes)
            for (auto& [b, nb] : nodes)
                if (a != b) (void)na->admit(NodeId{b}, cfg.cluster_id);
    }
    SwimMembership& s(std::uint64_t id) { return *nodes.at(id); }
    [[nodiscard]] std::int64_t now() const { return tick_no * tick_ns; }

    void round() {
        clk.now = tick_no * tick_ns;
        for (auto& [id, n] : nodes)
            if (n->online()) n->tick();
        fab.flush(max_waves, shuffle ? shuffle + static_cast<std::uint64_t>(tick_no) : 0, d,
                  (tick_no + 1) * tick_ns, clk);
        ++tick_no;
    }
    // True iff every online node's view holds exactly `n` members.
    [[nodiscard]] bool all_views_full(std::size_t n) {
        for (auto& [id, node] : nodes)
            if (node->online() && node->view().size() != n) return false;
        return true;
    }
};

}  // namespace quark::test
