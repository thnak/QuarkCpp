// Quark sample 13 — Cache affinity: "route the same user to the same GPU node so its warm cache is
// reused" — at (simulated) large scale, using ONLY Quark's real placement (010 HRW / 026 topology).
//
// THE IDEA. Placement is a PURE FUNCTION of actor identity: place(actor_id, node_set) -> node, the
// SAME answer on every caller with no coordinator and no routing table. So to make a user sticky to a
// node, you make the user's cache key BE the actor identity:
//
//     ActorId user_actor{TypeKey{"gpu-worker"}, hash(user_id)};   // identity == cache key
//     NodeId  node = *place(user_actor, gpu_nodes);               // always the same node for this user
//
// Every request for that user — from any front-end, any time — computes the same node independently
// and lands there, where the cache it warmed last time still lives. That is the whole mechanism.
// (The cache is either the actor's own in-memory state, or a node-local cache the actor reads because
// the actor always runs on that node.) At a million nodes this is O(1) via the 026 VirtualBins table;
// the math below is N-independent, so a modest N here demonstrates the same behavior.
//
// This program MEASURES the cache-hit rate under the cases that matter, and contrasts HRW with the two
// WRONG ways to route (naive hash%N, and a stateless round-robin pool).
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 13_cache_affinity
// Run  :  taskset -c 0-3 build/samples/13_cache_affinity
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/placement.hpp"
#include "quark/detail/hash.hpp"  // splitmix64 — model an identity-agnostic load-balancer (case 5)

using namespace quark;

// A user's actor identity == its cache key. Same user -> same ActorId -> same node.
static ActorId user_actor(std::uint64_t user_id) { return ActorId{TypeKey{0x6907'0000}, user_id}; }

int main() {
    bool ok = true;
    constexpr std::uint64_t N = 64;         // GPU nodes (the formula is N-independent; a million is the same)
    constexpr std::uint64_t U = 200'000;    // distinct users, each making repeated requests

    // A GPU cluster of N nodes.
    std::vector<NodeId> node_ids;
    for (std::uint64_t i = 1; i <= N; ++i) node_ids.push_back(NodeId{i});
    InProcessMembership cluster(NodeId{1}, std::vector<NodeId>(node_ids));

    // "Warm cache" model: which users each node has already processed. A HIT = the user's request lands
    // on a node that already warmed that user (cache reuse); a MISS = cold (must recompute + warm).
    auto owner = [&](std::uint64_t u, const MembershipView& v) { return place(user_actor(u), v)->value; };

    // ---- CASE 1: stable cluster — the same user always hits the same node. ----------------------
    {
        MembershipView v = cluster.view();
        std::vector<std::uint64_t> warmed_on(U);
        for (std::uint64_t u = 0; u < U; ++u) warmed_on[u] = owner(u, v);   // first request warms it
        std::uint64_t hits = 0;
        for (std::uint64_t u = 0; u < U; ++u)                               // every later request...
            if (owner(u, v) == warmed_on[u]) ++hits;                        // ...lands on the warm node
        ok &= (hits == U);
        std::printf("CASE 1  stable cluster:                repeat-request cache hit rate = %6.2f%%  (expected 100%%)\n",
                    100.0 * hits / U);
    }

    // ---- CASE 2: a GPU node dies — only ITS users lose their cache; everyone else stays warm. ----
    {
        MembershipView before = cluster.view();
        std::vector<std::uint64_t> warmed_on(U);
        std::uint64_t on_dead = 0;
        for (std::uint64_t u = 0; u < U; ++u) {
            warmed_on[u] = owner(u, before);
            if (warmed_on[u] == 7) ++on_dead;  // node 7 is about to die
        }
        InProcessMembership c2(NodeId{1}, std::vector<NodeId>(node_ids));
        c2.leave(NodeId{7});                                 // node 7 fails / drains
        MembershipView after = c2.view();
        std::uint64_t hits = 0, cold_from_move = 0;
        for (std::uint64_t u = 0; u < U; ++u) {
            const std::uint64_t now = owner(u, after);
            if (now == warmed_on[u]) ++hits; else ++cold_from_move;
        }
        // Only the ~U/N users that were on node 7 go cold; HRW never moves the others.
        ok &= (cold_from_move == on_dead);
        std::printf("CASE 2  1 of %llu nodes dies:            hit rate = %6.2f%%  (cold = the %llu users on the dead node, %.2f%% ~ 1/N)\n",
                    (unsigned long long)N, 100.0 * hits / U, (unsigned long long)cold_from_move,
                    100.0 * cold_from_move / U);
    }

    // ---- CASE 3: scale-up — add a node; only ~1/(N+1) users migrate (cold once), rest stay warm. -
    {
        MembershipView before = cluster.view();
        std::vector<std::uint64_t> warmed_on(U);
        for (std::uint64_t u = 0; u < U; ++u) warmed_on[u] = owner(u, before);
        InProcessMembership c3(NodeId{1}, std::vector<NodeId>(node_ids));
        c3.join(NodeId{N + 1});                              // add a fresh GPU node
        MembershipView after = c3.view();
        std::uint64_t hits = 0, moved = 0, moved_between_old = 0;
        for (std::uint64_t u = 0; u < U; ++u) {
            const std::uint64_t now = owner(u, after);
            if (now == warmed_on[u]) ++hits;
            else { ++moved; if (now != N + 1) ++moved_between_old; }
        }
        ok &= (moved_between_old == 0);  // HRW: users only ever move TO the new node, never between old ones
        std::printf("CASE 3  scale-up (add 1 node):          hit rate = %6.2f%%  (only %.2f%% ~ 1/(N+1) migrated, 0 moved between old nodes)\n",
                    100.0 * hits / U, 100.0 * moved / U);
    }

    // ---- CASE 4: the WRONG way #1 — naive hash % N collapses cache on ANY membership change. -------
    {
        auto mod_owner = [&](std::uint64_t u, std::uint64_t n) {
            return static_cast<std::uint64_t>(user_actor(u).hash() % n) + 1;  // node index 1..n
        };
        std::vector<std::uint64_t> warmed_on(U);
        for (std::uint64_t u = 0; u < U; ++u) warmed_on[u] = mod_owner(u, N);
        std::uint64_t hits = 0;
        for (std::uint64_t u = 0; u < U; ++u)
            if (mod_owner(u, N - 1) == warmed_on[u]) ++hits;   // one node leaves -> modulus changes
        std::printf("CASE 4  naive hash%%N, 1 node leaves:    hit rate = %6.2f%%  (CATASTROPHE — the modulus reshuffles almost everyone)\n",
                    100.0 * hits / U);
        ok &= (100.0 * hits / U) < 10.0;  // demonstrably terrible
    }

    // ---- CASE 5: the WRONG way #2 — a stateless pool routes by LOAD/rotation, not identity. ------
    // A user's two requests are uncorrelated in node, so the warm cache is almost never reused (~1/N).
    {
        std::uint64_t hits = 0;
        for (std::uint64_t u = 0; u < U; ++u) {
            const std::uint64_t req1 = detail::splitmix64(u) % N;            // where request 1 landed
            const std::uint64_t req2 = detail::splitmix64(u + 0x9E3779B9u) % N;  // request 2: independent
            if (req2 == req1) ++hits;                                        // hit only by coincidence
        }
        ok &= (100.0 * hits / U) < 5.0;  // ~1/N = 1.56% — cache-hostile
        std::printf("CASE 5  stateless load-balanced pool:  hit rate = %6.2f%%  (~1/N — balances load but CACHE-HOSTILE; wrong tool for affinity)\n",
                    100.0 * hits / U);
    }

    // ---- CASE 6: hotspot check — HRW is balanced, but a SINGLE hot identity pins to ONE node. ----
    {
        MembershipView v = cluster.view();
        std::vector<std::uint64_t> per_node(N + 2, 0);
        for (std::uint64_t u = 0; u < U; ++u) per_node[owner(u, v)]++;
        std::uint64_t mn = U, mx = 0;
        for (std::uint64_t i = 1; i <= N; ++i) { mn = per_node[i] < mn ? per_node[i] : mn; mx = per_node[i] > mx ? per_node[i] : mx; }
        std::printf("CASE 6  load spread over %llu nodes:      min=%llu max=%llu per node (balanced), BUT one hot user = one node -> shard its key (user+bucket) if it gets too hot\n",
                    (unsigned long long)N, (unsigned long long)mn, (unsigned long long)mx);
    }

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
