# 016 — Serialization

**Cross-cutting resolution.** One serialization mechanism serves three consumers:
network messages (010), state snapshots (012), and event-source logs (012). This
spec defines that mechanism and, critically, resolves the tension between them —
because "one story" cannot mean "one byte layout."

> **Status: Accepted (x86-64).** The promotion gate — the tagless wire fast-path
> encode budget + the negotiation/evolution correctness — is met:
> [ADR-016](decisions/ADR-016-serialization-wire-fast-path-encode-gate.md) proved it in
> all four build cells ({g++ 14.2, clang 20.1} × {-O2, -O3}). Tagless encode of a 24 B POD
> into a caller buffer: **p99 25–28 ns** — ~20× under the ≤500 ns Hard ceiling and inside the
> ≤200 ns goal — **near-memcpy** (0.85–1.06× of a same-byte `memcpy`), **0 heap alloc** over
> 1.2×10⁶ encodes, **reflection-free** under `-fno-rtti` (0 RTTI symbols in the codec TU).
> Round-trip, additive schema evolution (both directions), and a v1→v2→v3 migration chain all
> pass under clean ASan/UBSan. All three mandatory controls fired non-vacuously: a
> fingerprint-mismatch peer **corrupts** under forced-tagless (wrong value + ASan
> heap-overflow), an endian/ABI-mismatch peer **corrupts** (byte-swapped id) — proving the
> negotiation fallback is load-bearing — and the tagged path is 1.67–1.83× slower than
> tagless (the fast path is not vacuous). **Scope: x86-64** — the tight ≤200 ns *goal*-stamp
> defers to the 023 reference silicon (the Hard ceiling met on this sub-reference 2.10 GHz box
> implies the faster reference core meets it — headroom is one-directional), and the
> ARM64/big-endian cross-peer path (its fallback is proven load-bearing) awaits its own
> weak-memory/endianness re-gate.

## The tension (why a single physical encoding is the wrong goal)

| Consumer | Byte lifetime | Reader | Evolution pressure |
|---|---|---|---|
| **Wire** (010) | milliseconds | a peer node, usually the same binary | low (matched or rolling-upgrade binaries) |
| **Snapshot** (012) | until next snapshot | future self, possibly newer binary | high — state outlives code |
| **Event log** (012) | durable, forever | future self, definitely newer binary | **highest** — events are the source of truth, replayed after years |

A raw-memcpy-of-struct encoding is ideal for wire (near-free) and **catastrophic**
for durable data: the layout is ABI/compiler-dependent, and adding a field makes
every old byte unreadable. So the durable consumers demand *schema evolution*,
which the wire fast path cannot provide. The resolution is not one layout — it is
**one description, from which the engine derives the right encoding per context.**

## One description, reflection-free

A type describes its fields **once**. Everything — serialize, deserialize, schema
fingerprint, evolution — is generated from that single description. No reflection,
no RTTI:

```cpp
QUARK_SERIALIZE(Order,
    (1, id),          // (stable tag, member)
    (2, customer),
    (3, lines));
```

The macro expands to a `describe(Archive&, Order&)` visitor over `(tag, member)`
pairs. `Archive` is instantiated as a writer, a reader, or a fingerprint folder —
the same field list drives all of them. This is the whole trick that makes it
reflection-free: the developer writes the field list, not the codec.

### Field identity is the tag, not the position

Each field carries a **stable numeric tag**. This is the pivot that enables
evolution:

- **Add a field** → new tag + a default; old readers skip the unknown tag, new
  readers default the missing tag. Backward *and* forward compatible.
- **Remove a field** → retire its tag, never reuse it (reserved).
- **Rename a field** → free; names are cosmetic, tags are identity.
- **Reorder fields** → free; position carries no meaning.
- **Change a field's type** → breaking; requires a *new* tag + a migration (below),
  never an in-place type change.

### Alternatives considered (field identity)

- **Positional / declared-order encoding**: most compact and fastest, but adding
  or reordering a field breaks all existing data — unusable for durable logs.
- **Decision: tagged.** The overhead is **per field, not per byte** (a large
  trivially-copyable member is one length-delimited field, copied in bulk under a
  1–2 byte header), so tagging costs little even for big payloads while buying
  evolution everywhere it's needed.

## Canonical tagged encoding

The single canonical form is a minimal TLV stream. Each field is
`key = (tag << 3) | wire_type` (varint) followed by its value:

| Wire type | Encodes |
|---|---|
| `VARINT` (LEB128) | ints, bools, enums |
| `FIXED32` / `FIXED64` | floats/doubles, fixed-width integers |
| `BYTES` (length-delimited) | strings, blobs, arrays, **nested described types** (their own TLV stream) |

Canonical byte order is **little-endian**; big-endian hosts byte-swap at the
boundary. Deserialize dispatches each tag to its field via a small generated
sorted lookup — consistent with the dispatch philosophy in 008.

### Alternatives considered (reinventing protobuf)

The `key` encoding and wire-type set are deliberately protobuf's proven model.
We implement a **minimal subset ourselves** (a few hundred lines, std-only) rather
than depend on protobuf, to honor the low-dependency goal. Teams that already run
protobuf/FlatBuffers/Cap'n Proto plug them in behind the same `Serializer` seam
(010) — the adapter is optional and never linked into a minimal build.

## Schema version and fingerprint

Every serializable type has both:

- a **fingerprint** — a `constexpr` hash over its `(tag, wire_type)` set, computed
  automatically; and
- an explicit **`schema_version`** — a developer-bumped integer, changed on a
  breaking evolution.

They do different jobs:

| Use | Which | Where |
|---|---|---|
| Wire schema negotiation | fingerprint | peers exchange per-`type_key` fingerprints at connect (010) |
| Durable record header | `schema_version` | every snapshot/event record is prefixed `{type_key, schema_version}` (012) |
| Drift / unknown-version detection | both | Validation (008) fails fast on an unreadable persisted version |

## Wire fast path (a transparent optimization, not a second format)

Wire is the one consumer that can *sometimes* skip tags. At connect, peers compare
per-type fingerprints:

- **Fingerprints match *and* the peers share an ABI/endianness tag** → both ends
  agree on schema *and* memory layout, so the connection uses a **tagless packed
  encoding** (field values in tag order, no keys; trivially-copyable runs
  bulk-copied). Near-memcpy speed.
- **Otherwise** — schema mismatch (rolling upgrade) *or* a cross-platform peer of
  different endianness/ABI (a mixed Linux/Windows/ARM/x86 cluster) → automatic
  fallback to the canonical tagged, little-endian form, plus a warning (009).

Because the tagless path bulk-copies raw layout, it is only safe between
same-ABI, same-endianness peers; the connect-time negotiation therefore compares a
platform/ABI tag in addition to the schema fingerprint. The canonical tagged form
(always little-endian, layout-independent) is what makes a heterogeneous cluster
work at all.

This is invisible to developers — the same `describe` drives it, and durable data
**never** uses the tagless path. A message type used durably must have a
`describe`; using a non-described type in a persistent actor is a **Validation
error** (008). A purely local, never-persisted message needs no `Serializer` at
all.

## Migrations (evolution beyond additive rules)

Additive changes (add/remove/rename/reorder) need no code. A **breaking** change —
a field's type or meaning changes — is handled by a registered upcaster:

```cpp
QUARK_MIGRATE(Order, /*from*/ 2, /*to*/ 3, [](OrderV2 old) -> Order { … });
```

On read, if a durable record's `schema_version` is below current, the migration
chain is applied `v → v+1 → … → current` before the value reaches a handler.
Event logs rely on this most: events are immutable and long-lived, so old event
shapes must remain readable indefinitely. Snapshots can also be rewritten forward
opportunistically at the next checkpoint to bound chain length.

## Dependencies

Std-only: the TLV codec, LEB128, and `constexpr` fingerprinting are self-contained.
protobuf/FlatBuffers/Cap'n Proto are optional adapters behind the `Serializer`
seam.

## Open questions

- Whether `schema_version` should be inferred from the fingerprint history
  (tooling-assisted) rather than hand-bumped, to prevent forgotten bumps.
- Interning of repeated strings/`type_key`s within a large snapshot to cut size.
- Cross-language readers for durable logs (if non-C++ tools must read event logs) —
  the canonical TLV is language-neutral, but the migration chain is C++ code.
