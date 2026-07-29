# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Quark is a header-first C++23 actor engine (mailbox, work-stealing scheduler, hybrid sync/async
handlers, cluster distribution, persistence, supervision). The project is spec-driven: 28 RFC
documents (`NNN-*.md`) are the authoritative design, `decisions/ADR-*.md` are executed proofs
(real C++23 built under GCC+Clang, run under ASan/UBSan/TSan, benchmarked) that settle contested
hot-path/safety choices. **When code and a spec disagree, the spec wins**; if the spec is wrong,
fix the spec first (with an ADR), then the code.

Read **[CONVENTIONS.md](CONVENTIONS.md)** before writing any code — it is the binding coding
contract (target/scope, language rules, hot-path invariants, naming, layout). Read
**[README.md](README.md)** for the feature/status overview and the ADR index.

## Machine safety — read this first

**This dev box has 32 cores and can hang or power off if a build/test/bench saturates them.**

- Build with `cmake --build build -j4` — **never** `-j$(nproc)` / unbounded `make`. TSan builds: `-j1`.
- Run tests/benches pinned: `taskset -c 0-3 ctest --test-dir build ...`; single-thread microbenchmarks
  on one core (`taskset -c 0 ...`).
- Never spawn thread counts equal to `hardware_concurrency()`; multi-thread stress caps at 4 threads.

This is a hard rule, not a suggestion — it applies to every command below.

## Common commands

```bash
# Configure + build (Release is the default build type)
cmake -S . -B build
cmake --build build -j4

# Run the full correctness suite (auto-discovered *_test.cpp, one CTest target each)
taskset -c 0-3 ctest --test-dir build -j4 --output-on-failure

# Run a single test
taskset -c 0-3 ctest --test-dir build -R <test_name> --output-on-failure
# or run the binary directly:
taskset -c 0-3 build/tests/<test_name>

# Sanitizer builds (separate build dirs; same test suite minus by-design exclusions, see VERIFICATION.md)
cmake -S . -B build-asan -DQUARK_SANITIZE="address;undefined" -DCMAKE_BUILD_TYPE=Debug
cmake -S . -B build-tsan -DQUARK_SANITIZE="thread" -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan -j1   # TSan compiles are memory-heavy — serial build only

# Benchmarks (ON by default) — the 023 performance-budget gate
cmake --build build --target bench-gate     # FAILs on a Hard-budget [MISS], WARNs on a Goal regression
taskset -c 0 build/bench/<name>_bench       # single-bench run, pinned

# Runnable samples (OFF by default)
cmake -S . -B build-samples -DQUARK_BUILD_SAMPLES=ON
taskset -c 0-3 build-samples/samples/01_hello_counter

# Opt-in persistence adapters (std-only core needs neither)
cmake -S . -B build -DQUARK_WITH_SQLITE=ON -DQUARK_WITH_ROCKSDB=ON
```

Compiles clean under **g++ 14.2** and **clang++ 20.1**, `-std=c++23 -Wall -Wextra`. New test files
(`*_test.cpp` in `tests/`) and benches (`*_bench.cpp` in `bench/`) are picked up by `CONFIGURE_DEPENDS`
glob — re-run cmake (or build, which reconfigures) after adding one.

## Architecture

```
include/quark/core/     header-first engine core — hot path lives here (mailbox, scheduler,
                         dispatch, streams, persistence, supervision, cluster, security)
include/quark/net/      default TCP transport + wire codec
include/quark/adapters/ opt-in persistence backends (SQLite, RocksDB) — never linked by default
include/quark/detail/   private internals (message pool, reply cell, hashing) — not user-facing
pal/                    Platform Abstraction Layer — the ONLY OS/arch seam (spec 019);
                         pal/pal.hpp + linux_x86_64/ and windows_x86_64/ backends
src/                    non-template translation units (the core is otherwise header-only)
tests/                  correctness gate, one *_test.cpp per load-bearing invariant (CTest)
bench/                  hot-path microbenchmarks, percentiles vs the 023 budget table
samples/                runnable programs over the public developer surface (01..22, numbered by topic)
decisions/              ADRs — design → red-team → prove → judge verdicts, most on the mailbox hot path
NNN-*.md                the 28 RFC specs (authoritative design; see README's reading-order table)
```

Core concepts (see the glossary in README.md for the full list): an **Actor** is state + sequential
behavior addressed by id; an **Activation** is the (at most one) right to execute it; a **Worker**
(thread) borrows activations from a **Shard**; the **Mailbox** is an intrusive Vyukov MPSC queue
where the queue node *is* the message descriptor (spec 003, proven in ADR-002/003/004/020/027/029/
031/032/033 — the mailbox hot path has had the most rounds of red-team/prove iteration in the
project, see the ADR table in README.md). Policies (`Sequential`, `Priority<P>`, `Placement<…>`,
`DrainBudget<N>`, ...) are CRTP template parameters, not runtime config — they resolve to metadata
at startup with zero runtime cost (ADR-007's JumpTable-Dispatch).

Every subsystem that would otherwise need a heavy dependency is a seam with a std-only default and
an optional adapter (transport, serialization, membership, persistence, metrics — see the
"Dependency posture" table in README.md); the core itself has no dependencies beyond the standard
library.

## Working within this repo

- **Every source file names the spec(s)/ADR(s) it implements** in a top comment
  (`// Implements 003-Memory §Mailbox — Vyukov MPSC; ADR-002/003/004 hot path.`) — read that comment
  before editing a file to know which spec/ADR governs it.
- **A load-bearing invariant without a test, or a hot path without a bench, is not done** — hot-path
  rules (0 heap allocations, 0 cross-core RMW on the drain path, descriptor ≤ 64 B, single-executor
  invariant) are enforced by tests/benches, not comments.
- Error handling is `quark::result<T> = std::expected<T, quark::error>` — no exceptions for control
  flow, hot path is `noexcept`. No RTTI/`virtual`/reflection on the hot path (checked by ADR-016's
  0-RTTI-symbol codec-TU gate).
- Contesting a hot-path or safety-critical design goes through `.claude/agents/quark-architect`,
  `quark-redteam`, `quark-prover`, `quark-judge` via the `design-debate-prove` workflow
  (`.claude/workflows/`) — it produces the next `decisions/ADR-*.md`, not an ad-hoc code change.
- `VERIFICATION.md` is the correctness record (test counts, sanitizer exclusions and why);
  `PERFORMANCE.md` is the full benchmark report; `OpenQuestions.md` tracks unresolved cross-cutting
  design questions. Check these before assuming a gap is unnoticed.
