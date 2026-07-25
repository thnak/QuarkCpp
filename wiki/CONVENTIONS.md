# Quark implementation conventions — the coding contract

This file binds all code (human- or agent-written) to the RFC specs. **Every change must
follow the specs (`001`–`026`) and the proven decisions (`decisions/ADR-*`).** When code and
a spec disagree, the spec wins; if the spec is genuinely wrong, fix the spec first (RFC-style,
with the ADR that proved it), then the code — never silently diverge.

## Target & scope

- **Primary target: Linux / x86-64.** This is the only supported, verified platform for now.
- **Cross-platform by construction, but deferred.** All OS/arch specifics live behind the
  **PAL** (`pal/`, spec 019). Only the `linux_x86_64` backend exists today. Windows/macOS and
  ARM64 come later — do not add them speculatively, but never bake an OS/arch assumption into
  the core; route it through a PAL seam. Anything proven only on x86-TSO (the `seq_cst` Dekker
  close-out, producer-fence elision, fiber handshakes, big-endian serialization) carries a
  deferred ARM64 weak-memory re-gate — leave a `// TODO(arm64):` marker at the seam.

## Language & dependencies

- **C++23, std-only core.** `std::expected`, `<coroutine>` (`quark::task<>`), `std::stop_token`,
  `std::pmr` shard allocators, concepts + deducing-this (`this auto&&`). Heavy dependencies
  (protobuf, io_uring wrappers, etc.) are **optional adapters behind a seam** (e.g. `Serializer`,
  `Membership`, `Transport`) — never in the core, never a core `#include`.
- **No .NET / managed-runtime vocabulary** anywhere — names, comments, or design. Idiomatic C++.
- **No RTTI, no reflection, no `virtual` for policy on the hot path.** Policies are CRTP /
  compile-time (`Actor<Derived, Sequential>`), dispatch is the ADR-007 jump table. `typeid` /
  `dynamic_cast` are banned in core translation units (codec TU must show 0 RTTI symbols, ADR-016).
- **Error model: `quark::result<T> = std::expected<T, quark::error>`.** No exceptions for control
  flow; the hot path is `noexcept`. Exceptions may surface only from cold setup paths.

## Hot-path rules (these are tested, not trusted — spec 023)

- **0 heap allocations on the hot path** (measured with a hooked allocator, not asserted).
- **0 cross-core atomic RMW on the sequential drain path** (per-shard single-writer; measured).
- **Descriptor + handle ≤ 64 B** (one cache line); payload stored separately (003).
- **Single-executor invariant**: at most one executor per actor at any instant. Mailbox FIFO by
  default. The scheduler schedules **activations**, never reorders one actor's messages.
- Preserve the settled mechanisms verbatim where an ADR proved them: the Vyukov intrusive MPSC
  mailbox + packed 48-bit-generation `gen_state` word (ADR-002/003/004), the exec-state wakeup +
  `seq_cst` Dekker close-out (ADR-002/004), the `Parked` seal for blocking/fiber adapters (ADR-015).

## Layout

```
include/quark/          public headers (what users include)
  core/                 001 execution, 002 scheduler, 003 memory/mailbox, hot path
  detail/               private helpers (not user-facing)
src/                    non-template implementation units
pal/                    019 platform abstraction — pal.hpp (seam) + linux_x86_64/ backend
tests/                  correctness (one per load-bearing invariant); CTest
bench/                  hot-path microbenchmarks (percentiles, spec 023 budgets)
<NNN>-*.md              the specs (authoritative);  decisions/ADR-*.md  the proofs
```

## Naming & files

- Namespace `quark`; private helpers in `quark::detail`; platform in `quark::pal`.
- **Types** `PascalCase`, **functions/members/variables** `snake_case`, **macros** `QUARK_UPPER`
  (matches the spec code samples, e.g. `class Order : public Actor<Order, Sequential>`,
  `quark::task<> handle(const Query&)`, `QUARK_SERIALIZE(Order, (1, id), ...)`).
- **Every source file names the spec(s)/ADR(s) it implements in a top comment**, e.g.
  `// Implements 003-Memory §Mailbox — Vyukov MPSC; ADR-002/003/004 hot path.`
- Header guards: `#pragma once`.

## Build & test — **machine safety: cap at 4 cores**

This dev box has 32 cores and **can hang or power off if a build/benchmark saturates them.**

- Configure/build with **`-j4` MAX** — never `-j$(nproc)` / `-j32` / unbounded `make`.
  `cmake --build build -j4`.
- **Pin every test and benchmark to ≤ 4 cores**: `taskset -c 0-3 ...`; single-thread microbenchmarks
  to one core (`taskset -c 0 ...`). Never spawn `std::thread` counts == `hardware_concurrency()`;
  multi-thread stress ≤ 4 threads. Per-core ns / percentile numbers stay valid under the cap.
- Compile clean under **both g++ 14 and clang++ 20**, `-std=c++23 -Wall -Wextra`.
- Correctness tests run under **ASan/UBSan**, and **TSan** for anything with cross-thread edges.
- A load-bearing invariant without a test, or a hot path without a bench, is not done.
