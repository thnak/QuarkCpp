# Quark samples

Small, runnable programs that show how to **author actors** and **drive the engine** through the
public developer surface (spec 005 developer model + 006 messaging). They are *not* part of the
engine and *not* a correctness gate — the `tests/` suite is that. These exist to be read and run.

Read them in order: the basics come first, each the previous one plus a single new idea; the advanced
set covers one subsystem apiece.

**Basics — authoring & driving actors**

| # | Sample | Idea | Specs |
|---|--------|------|-------|
| 01 | [`01_hello_counter`](01_hello_counter/main.cpp) | one actor; `tell` mutates state, `ask` reads it back | 001, 005, 006 |
| 02 | [`02_supervised_worker`](02_supervised_worker/main.cpp) | a throwing handler is contained; a poison `ask` returns an error, never hangs | 007 |
| 03 | [`03_pipeline`](03_pipeline/main.cpp) | an actor holds an `ActorRef` and forwards to a downstream actor | 006 |
| 04 | [`04_scheduled_timer`](04_scheduled_timer/main.cpp) | one-shot + periodic timers deliver as messages; `cancel()` stops them | 011 |

**Advanced — one subsystem each**

| # | Sample | Idea | Specs |
|---|--------|------|-------|
| 05 | [`05_cooperative_cancellation`](05_cooperative_cancellation/main.cpp) | `quiesce(Cancel)` fires `stop_token`s; in-flight siblings unwind cooperatively before state reset | 015 |
| 06 | [`06_streaming_backpressure`](06_streaming_backpressure/main.cpp) | bounded credit ring: producer throttles losslessly when credit runs out; FIFO, no loss/dup | 024 |
| 07 | [`07_persistence`](07_persistence/main.cpp) | snapshot round-trip, event-sourced replay, and fencing (split-brain rejection) | 012 |
| 08 | [`08_cluster_two_nodes`](08_cluster_two_nodes/main.cpp) | two nodes over a loopback fabric; local fast path (no wire) vs remote (016 over transport) | 010 |
| 09 | [`09_placement`](09_placement/main.cpp) | rendezvous/HRW placement: deterministic, balanced; the O(1) `VirtualBins` quantized table | 010, 026 |
| 10 | [`10_replacement`](10_replacement/main.cpp) | minimal-disruption re-placement on node join/leave (~1/N moves, only the right actors) | 010 |
| 11 | [`11_stateless_workers`](11_stateless_workers/main.cpp) | `Stateless<N>` worker pool with load-balanced, exactly-once routing | 025 |
| 12 | [`12_capability_actor`](12_capability_actor/main.cpp) | capability-constrained placement: `Require`/`Prefer`/`Weighted` on typed node capabilities (GPU, zone, capacity) | 025 |
| 13 | [`13_cache_affinity`](13_cache_affinity/main.cpp) | "same user → same GPU node" so a warm cache is reused: identity-as-cache-key + HRW stickiness, measured vs hash%N and a load-balancer | 010, 026 |
| 14 | [`14_durable_reminders`](14_durable_reminders/main.cpp) | durable wall-clock reminders (SEGSTREAM): one-shot + periodic, survival across a simulated restart, and the 9 PM mass-due wave flattening to `peak == fire_rate` instead of a spike | 027 |

## Build & run

Samples are gated behind an off-by-default CMake option so the test/sanitizer builds stay lean:

```bash
cmake -B build -DQUARK_BUILD_SAMPLES=ON
cmake --build build -j4                      # -j4, never -j$(nproc) — see the machine-safety note

# Run any sample, pinned to <=4 cores (never saturate the box):
taskset -c 0-3 build/samples/01_hello_counter
taskset -c 0-3 build/samples/07_persistence
taskset -c 0-3 build/samples/08_cluster_two_nodes
# ... every binary is under build/samples/<name>
```

Each prints its results and exits `0` on success (`OK`) / `1` on failure (`FAIL`), so they double as
smoke checks. To build just one: `cmake --build build --target 03_pipeline`.

## The shape every sample shares

```cpp
// 1. Declare an actor: CRTP base carries policies; `protocol` is the closed message set;
//    one handle() overload per message.
struct MyActor : quark::Actor<MyActor, quark::Sequential> {
    using protocol = quark::Protocol<Msg, quark::Ask<Query, Reply>>;
    void handle(const Msg&) noexcept { /* mutate state */ }
    void handle(const quark::Ask<Query, Reply>& m) noexcept { m.respond(/* reply */); }
};

// 2. Bring-up: pool -> activation -> engine -> register -> router -> ref.
quark::detail::MessagePool pool(1024);
MyActor actor;
auto act = std::make_unique<quark::Activation>(&actor, MyActor::dispatch_table(), pool.sink());
quark::Engine<> eng(quark::EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64});
eng.register_activation(quark::actor_id_of<MyActor>(/*key*/ 1), *act);
quark::LocalRouter router(eng.post_courier(), pool);
quark::ActorRef<MyActor> ref = router.get<MyActor>(1);
eng.start();

// 3. Drive it.
ref.tell(Msg{...});                                   // fire-and-forget
quark::result<Reply> r = quark::block_on(ref.ask<Reply>(Query{...}));  // request/reply
eng.stop();
```

`register_actor<MyActor>(eng, key, *act)` (used in sample 01) is the same registration but resolves
the scheduling band/budget from the actor's policies instead of taking a raw activation — either path
gives you a `Schedulable` the router can address by `key`.

## Machine-safety note

This dev box can hang or power off if a build/run saturates all cores. Build with `-j4` (never
`-j$(nproc)`), and run every sample under `taskset -c 0-3`. The samples are single- or few-threaded by
design and never call `std::thread::hardware_concurrency()`.
