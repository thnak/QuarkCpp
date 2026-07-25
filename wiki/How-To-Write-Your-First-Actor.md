# How To: Write Your First Actor

A walkthrough of the smallest complete Quark program: one actor, driven by `tell`
(fire-and-forget) and `ask` (request/reply) over the real engine. It mirrors
[sample 01](https://github.com/thnak/QuarkCpp/blob/master/samples/01_hello_counter/main.cpp)
(`01_hello_counter`) line for line — clone the repo and build that target if you'd
rather read runnable code than a tutorial.

For the concepts behind each step, see [005-Developer-Model](005-Developer-Model)
(actor authoring), [006-Messaging-and-Addressing](006-Messaging-and-Addressing)
(`tell`/`ask`), and [001-Actor-Execution-Model](001-Actor-Execution-Model)
(activation/lifecycle).

## Prerequisites

CMake ≥ 3.24 and a C++23 compiler (verified: g++ 14.2, clang 20.1) — see
[Home](Home#quick-start) for the full build setup. Samples are opt-in:

```bash
cmake -B build -DQUARK_BUILD_SAMPLES=ON
cmake --build build --target 01_hello_counter
taskset -c 0-3 build/samples/01_hello_counter   # pin to <=4 cores, never saturate
```

## 1. Declare the messages

Messages are plain structs — no base class, no macros. A `tell` carries one; an
`ask` pairs a query type with a reply type via `Ask<Query, Reply>`.

```cpp
struct Add {
    int amount;
};
struct GetTotal {};
```

## 2. Declare the actor

```cpp
struct Counter : Actor<Counter, Sequential, Priority<0>, DrainBudget<16>> {
    using protocol = Protocol<Add, Ask<GetTotal, int>>;

    void handle(const Add& a) noexcept { total_ += a.amount; }
    void handle(const Ask<GetTotal, int>& m) noexcept { m.respond(total_); }

private:
    int total_ = 0;
};
```

- The first template argument is the actor's own type (CRTP); the rest are
  **policies** — compile-time metadata, not runtime configuration. Here:
  `Sequential` (one message at a time, the default), `Priority<0>` (scheduling
  band), `DrainBudget<16>` (max messages drained before yielding the worker).
  An actor with no policies at all (`Actor<Counter>`) is valid and sequential.
  Full policy catalog: [005-Developer-Model § Policy catalog](005-Developer-Model#policy-catalog).
- `using protocol = Protocol<…>` is the **closed set** of messages this actor
  dispatches. Every type listed here needs a matching `handle` overload, and
  vice versa — mismatches are a **compile error at the send site**, not a
  runtime surprise ([005 § Validation](005-Developer-Model#validation-fail-fast)).
- `handle(const Add&)` is a **tell handler** — no reply, mutates state in place.
- `handle(const Ask<GetTotal, int>&)` is an **ask handler** — call `m.respond(value)`
  to answer the caller. (An actor can also opt a specific message into an async
  handler by returning `quark::task<>`; see [005](005-Developer-Model#defining-an-actor).)
- Dispatch is a dense compile-time jump table (no RTTI, no virtual call) — see
  [005 § Handler dispatch](005-Developer-Model#handler-dispatch) for how.

## 3. Bring up the engine

```cpp
detail::MessagePool pool(1024);
Counter counter;
auto activation = std::make_unique<Activation>(&counter, Counter::dispatch_table(), pool.sink());

Engine<PriorityBands<2>> eng(EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64});

register_actor<Counter>(eng, /*key*/ 42, *activation);

LocalRouter router(eng.post_courier(), pool);
ActorRef<Counter> counter_ref = router.get<Counter>(42);
eng.start();
```

- `MessagePool` backs zero-allocation message envelopes.
- `Activation` binds the actor instance to its compile-time dispatch table.
- `Engine<PriorityBands<2>>` owns the worker lane(s). `EngineConfig{workers,
  shards, drain_budget, busy_spin_limit}` is **structural** — set once at
  construction, never mutated live. Always pass `workers` explicitly; never
  size it off `hardware_concurrency()` (see the machine-safety note on
  [Home](Home#quick-start)).
- `register_actor<Counter>(eng, key, activation)` resolves Counter's
  band/budget from its policies and binds it to key `42` — the identity a
  matching `router.get<Counter>(42)` will look up later.
- `LocalRouter` hands out typed `ActorRef<Counter>` handles for local sends.
  `eng.start()` brings the worker(s) up; nothing runs before this.

## 4. Send messages

```cpp
// tell: fire-and-forget, no reply, no blocking, no hot-path allocation.
for (int i = 1; i <= 100; ++i) counter_ref.tell(Add{i});

// ask: request/reply. Sequential execution + mailbox FIFO means this
// observes every tell sent before it. block_on drives the caller side
// until the reply (or an error) resolves.
result<int> total = block_on(counter_ref.ask<int>(GetTotal{}));
```

State persists across messages on the same actor instance — a second batch of
`tell`s keeps accumulating on top of the first:

```cpp
counter_ref.tell(Add{1000});
result<int> total2 = block_on(counter_ref.ask<int>(GetTotal{}));
// total2 == 6050
```

`result<T>` is `std::expected`-shaped — check `.has_value()` before `.value()`;
an `ask` can resolve to an error (e.g. actor gone, deadline exceeded).

## 5. Shut down

```cpp
eng.stop();
```

Always stop the engine before the program exits — it drains outstanding work
and joins the worker threads.

## Full source

The complete, runnable file (with the `printf` checks and exit code) is
[`samples/01_hello_counter/main.cpp`](https://github.com/thnak/QuarkCpp/blob/master/samples/01_hello_counter/main.cpp).

## Next steps

| Want to... | See |
|---|---|
| Forward messages between actors via `ActorRef` | [Samples](Samples) — sample 03 (`03_pipeline`) |
| Handle a failing handler without crashing the actor | [Samples](Samples) — sample 02 (`02_supervised_worker`), [007-Failure-and-Supervision](007-Failure-and-Supervision) |
| Add an async (`quark::task<>`) handler | [005-Developer-Model](005-Developer-Model#defining-an-actor) |
| Understand `tell`/`ask` semantics in depth | [006-Messaging-and-Addressing](006-Messaging-and-Addressing) |
| See every policy an actor can declare | [005-Developer-Model § Policy catalog](005-Developer-Model#policy-catalog) |
| Run an actor across a cluster instead of one process | [Samples](Samples) — samples 08/15/16, [010-Distribution](010-Distribution) |
