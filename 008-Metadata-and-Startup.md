# 008 — Metadata and Startup

Defines the pipeline that turns registered actor types into runtime dispatch
tables, and the "fail-fast at startup" guarantee. This is where reflection is
*avoided*: everything is derived from CRTP policies and concepts at compile time,
then flattened into arrays read on the hot path.

```
Discovery → Validation → Metadata compilation → Runtime
```

## Discovery

Actors are **explicitly registered** with a builder before the engine starts:

```cpp
auto engine = quark::EngineBuilder{}
    .register_actor<Order>()
    .register_actor<Inventory>()
    .configure(cfg)          // see 013
    .build();                // returns std::expected<Engine, ValidationReport>
```

An optional `QUARK_REGISTER(Order)` macro adds a type to a translation-unit
registry the builder can bulk-import, for projects that prefer declaration-site
registration.

### Alternatives considered

- **Static self-registration** (global registrar constructed at static-init).
  Rejected as the default: subject to the static-init-order fiasco, and it makes
  "which actors are in *this* engine instance" nondeterministic — hostile to
  fail-fast and to running two engines in one process (tests, 014).
- **Linker-section registration** (`__attribute__((section))`). Rejected:
  compiler/linker-specific, hard to make portable across the three target
  toolchains.
- **Decision:** explicit builder registration is the source of truth; the macro
  is sugar over it, not a parallel mechanism.

## Type identity

Every actor type and every message type gets **two** ids, derived without RTTI:

| Id | Type | Derivation | Used for |
|---|---|---|---|
| `type_key` | `uint64_t` | `constexpr` FNV-1a hash of a canonical type name | Stable across runs/binaries → wire (010) & storage (012) keys |
| `type_index` | `uint16_t` | Dense counter assigned at registration | Array indexing on the hot path |

Two ids because the requirements conflict: distribution and persistence need an id
that is **identical on every node and every restart** (`type_key`), while dispatch
tables want a **dense, small** index for O(1) array lookup (`type_index`). The
canonical name is derived from a `constexpr` view of the compiler's function-name
string, normalized to strip compiler-specific decoration.

Collision handling: `type_key` collisions are detected during Validation (two
distinct registered types hashing equal) and are a **Strict-mode startup failure**.

## Validation

Validation is split by *when the fact is knowable*:

- **Compile time** — malformed policy lists, an actor that declares no `handle`,
  a `Sequential`+`Reentrant` conflict: `static_assert` / concept failure. These
  never reach runtime.
- **Startup time** — facts that depend on configuration or the full type set:
  missing resource providers (004), placement referencing an unknown node (010),
  `type_key` collisions, config out of range (013), a persistent actor whose
  state/message types lack a `describe` or carry an unreadable persisted
  `schema_version` (012, 016). These return

```cpp
std::expected<EngineMetadata, ValidationReport>
```

`ValidationReport` is a list of `{severity, code, subject, message}`. Modes:

- **Strict** (default) — any error fails `build()`; the engine never starts.
- **Relaxed** — errors are downgraded to warnings emitted through the log/metric
  sinks (009) and the engine starts with degraded actors quarantined.

## Metadata compilation

For each registered actor, a `ActorMetadata` record is materialized once:

```cpp
struct ActorMetadata {
    quark::type_key    key;
    quark::type_index  index;
    ConstructFn        construct;      // build actor + wire Cached<> resources (004)
    ResourcePlan       resources;      // activation resolutions + per-message factories
    PlacementFn        place;          // ActorId → shard/node (002, 010)
    ExecutionPolicy    exec;           // Sequential / Reentrant / MaxConcurrency
    SchedulingPolicy   sched;          // priority, drain budget
    LifecyclePolicy    life;           // keep-alive, idle timeout
    FailurePolicy      failure;        // 007
    DispatchTable      dispatch;       // dense-slot .rodata thunk array (local path)
                                       //   + sorted type_index→thunk scan (wire path)
};
```

Records live in a flat array indexed by `type_index`; a parallel sorted array of
`type_key → type_index` handles wire/storage lookups.

### Dispatch table

Resolved by [ADR-007](decisions/ADR-007-actor-authoring-and-handler-dispatch-api.md)
(D1 JumpTable-Dispatch). There are **two** structures, materialized at metadata
compilation, chosen by whether the message type is statically known at the send site:

1. **Primary local path — per-actor dense-slot array.** A
   `static constexpr std::array<Thunk, k>` in `.rodata`, indexed by the process-local
   `msg_slot_` the send site stamps into the descriptor (`slot_of<A,M>` is a
   `consteval` dense index over `using protocol = Protocol<…>`). The drain does **one
   indexed indirect call**, `thunks[msg_slot_](self, payload, ctx)` — no lookup, no
   hash, no scan. This beats the sorted-flat scan on the local path (proven) and is
   **≈ 260 B/actor, linear, independent of the engine-wide message count**. The dense
   slot is **process-local**, renumbers when handlers change, and is **never
   serialized**.
2. **Non-statically-typed path — sorted flat array.** A `(msg type_index, thunk)`
   sorted flat array with a branchless scan, retained **only** for messages whose type
   is not statically known at the send site: wire arrival (010) and any future untyped
   forward. Such a path recovers the thunk via the `type_index` scan; it never uses the
   process-local dense slot.

A thunk erases the concrete message type behind a stable call signature and encodes
sync vs. async (001).

### Alternatives considered (dispatch)

- **Sorted flat array + scan as the *primary* structure** (the earlier decision):
  cache-resident and allocation-free, but on the local path it does an O(k) scan where
  the dense-slot table does an O(1) indexed call. Demoted to the non-statically-typed
  path only.
- **Generated `switch`** over message ids: ties the dense-slot table on the hot path
  (uniform *and* skew) but requires the full message set at the actor's definition site
  and code-gen tooling. A future opt-in devirtualizing `switch` may be offered for a
  proven hot-single-type path.
- **CPO/`tag_invoke` pre-baked per-`(A,M)` thunk**: needs no protocol list, but its
  "zero indirect under monomorphic LTO" edge was **disproven** (one `call *0x10(reg)`
  remains on both toolchains — ADR-007), leaving it at one-indirect parity with the
  dense table but a larger per-`(A,M)` code-bloat tax.
- **`std::unordered_map<type_index, thunk>`**: pulls hashing + a node-based
  container onto a path we want allocation- and pointer-chase-free.
- **Decision:** dense-slot `.rodata` array for the local typed path (primary); sorted
  flat array + scan retained for the non-statically-typed path.

## Runtime

After compilation the engine holds only flat tables. **No reflection, no RTTI, no
policy re-derivation, no map lookups keyed by strings on the hot path.** A message
send resolves `type_index → ActorMetadata → dispatch`, all array indexing.

## Dynamic registration (guarded) — ADR-008

Resolved by [ADR-008](decisions/ADR-008-engine-actor-configuration-and-activation-lifecycle-policy.md):
a **guarded `add_actor_type<T>()`** is permitted after `build()`. The actor *type set* is
a BuildOnly knob for **sizing** (arrays are pre-sized to a `max_types` cap), but a new
type may be added within that cap:

- **Incremental Validation** against the frozen core — `type_key` collision scan versus the
  registered set, plus the new type's policy/resource concept checks. On failure it returns
  `std::unexpected(…)` and **publishes nothing**.
- A dense `type_index` is appended; the new type's `(shard × type_index)` operational cells
  (013) are initialized with a **release store**.
- A **new immutable metadata table** is published by a **single release pointer-swap**
  (acquire on the send path). Every pre-existing `ActorMetadata` record is
  **pointer-identical** across the swap — so cached `op_cell_` / metadata pointers held by
  live activations stay valid.
- `cells[]` / `warm[]` are **pre-sized to `max_types`** and never realloced; exceeding the
  cap returns `CapExceeded`. The append-only table is **QSBR-reclaimed** (grace =
  `in_flight == 0`).
- Proven: collision → `unexpected`, table pointer-identical; add-vs-drain TSan+ASan clean,
  0 bad records/cells.

## Dependencies

Std-only. Type-name hashing uses `constexpr` string handling over the compiler's
built-in function-name macro; no external reflection library.

## Open questions

- Canonical type-name normalization across GCC/Clang/MSVC — **mandatory** for the
  cross-platform target, since a mixed cluster (010) and shared durable logs (012,
  016) require every toolchain to derive an identical `type_key` for the same type.
  Needs a per-toolchain conformance test; if normalization proves unreliable, fall
  back to a developer-assigned explicit `type_key` per type.
- *(Hot-reload / dynamic registration after `build()`: resolved — a guarded
  `add_actor_type<T>()` with incremental Validation + release table swap, pre-sized to a
  `max_types` cap; see "Dynamic registration (guarded)" above, ADR-008.)*
