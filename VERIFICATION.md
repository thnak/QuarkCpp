# Quark — Verification (test-suite + sanitizers)

This is the **run-and-result** record for the correctness gate: the full `tests/` suite compiled and
executed under a Release build and under the sanitizers. It is the safety companion to
[PERFORMANCE.md](PERFORMANCE.md) (which records measured *speed*); this file records measured
*correctness* — no data races, no leaks, no undefined behavior, all invariants asserted green.

Every run below is reproducible with the commands shown. All builds and runs are **pinned to ≤4 cores**
(`taskset -c 0-3`) and the ThreadSanitizer build uses **`-j1`** — a machine-safety rule for this box,
which can hang or power off if a build/run saturates all cores. Never use `-j$(nproc)`.

## Latest run

| Field | Value |
|---|---|
| Commit | `a602f29` (merge of `netem-swim-chaos` into `master`) |
| Date | 2026-10-25 |
| Host | Linux x86-64, 28-core Xeon E5-2680 v4, 61 GB (`devserver`); Linux x86-64 remains the primary target — ARM64 is CI-verified and Windows is a correctness-only leg (CONVENTIONS.md) |
| Compilers | g++ 15.2.0 (Release + all sanitizers), clang 21.1.0 (second supported compiler) |
| Standard | C++23 |

| Config | Command (build dir) | Result |
|---|---|---|
| **Release** (gcc) | `build` | **211 / 211 passed**, 0 failed |
| **ThreadSanitizer** (gcc) | `build-tsan` | **194 / 194 passed**, 0 failed — no data races (the TSan config's documented exclusions; `tcp_transport_control_priority_test` passed this run) |
| **ASan + UBSan** (gcc 15.2, Linux) | `build-asan` | **Full suite, 2026-10-28 (ADR-062 + #10/#21 commits): 238 / 242 on first run, 2 real heap-use-after-free found and fixed (`FanOut` member teardown order; `delivery_at_least_once_test` re-entering `pump()` from its sink) — both re-run clean (commit `7d331fc`). The other 2 misses are the firing controls `activation_park_race_control` and `topic_no_quiesce_control`, which did not reproduce their race on this host (timing-dependent; untouched by these changes). The full suite was not re-run after the fixes, only the two tests. |
| **SWIM chaos** (not CTest) | `tests/netem/run_swim_chaos.sh build` | **5 / 5** scenarios end all-alive (form, isolate, asym, split, flaky) |

Release is the full gate — it is the only config where every allocation-counting probe and every
store-buffer litmus control is meaningful, so it runs the complete set. The sanitizer configs run a
*different-sized* set **by design**: a fixed, principled set of tests is excluded because sanitizer
instrumentation invalidates exactly what they measure, and one sanitizer-only firing control
(`topic_no_quiesce_control`) is *added* under ASan because it needs ASan's trap to be observable. The
deltas below are the *whole* difference — no test silently dropped out.

## Reproduce

```bash
# Release (gcc) — the full 211-test gate
cmake -S . -B build
taskset -c 0-3 cmake --build build -j4
taskset -c 0-3 ctest --test-dir build -j4 --output-on-failure

# Address + Undefined Behavior sanitizers
cmake -S . -B build-asan -DQUARK_SANITIZE="address;undefined"
taskset -c 0-3 cmake --build build-asan -j4
# ASAN_OPTIONS: the one deliberate-trap test (serialize_fingerprint_mismatch_oob) reaches ASan's
# stack symbolizer; this box has addr2line but no llvm-symbolizer, whose absence HANGS the default
# symbolizer spawn. symbolize=0 makes the trap robust; the trap itself still fires (WILL_FAIL).
ASAN_OPTIONS=symbolize=0:abort_on_error=0 \
  taskset -c 0-3 ctest --test-dir build-asan -j4 --output-on-failure

# ThreadSanitizer — build MUST be -j1 (a -j2 clean TSan compile OOM-kills the compiler on this box)
cmake -S . -B build-tsan -DQUARK_SANITIZE="thread"
taskset -c 0-3 cmake --build build-tsan -j1
taskset -c 0-3 ctest --test-dir build-tsan -j4 --output-on-failure
```

## Sanitizer exclusions (why the counts differ, and why that is correct)

The exclusion logic lives in [`tests/CMakeLists.txt`](tests/CMakeLists.txt) and is guarded per
sanitizer; each excluded test keeps full coverage in the configs where it *is* meaningful. Unlike a
name-based skip list, the TSan operator-new exclusion is **content-detected** (the CMake configure step
greps each test source for a global `operator new(std::size_t` replacement) — so a newly-added
allocation probe is caught automatically instead of silently breaking the TSan link.

### ASan/UBSan: 153 → 152 (net −1: −2 excluded, +1 added)

| Test | Direction | Why |
|---|---|---|
| `stream_close_out_dekker_control` | excluded | Pure store-buffer-reorder litmus (NO_FENCE variant). ASan's shadow-memory checks perturb instruction scheduling enough to drain the store buffer before the racing load, so the reorder cannot manifest — the control can't produce its intended FIRE. Verified in the gcc/clang builds instead. |
| `stream_drain_zero_rmw` | excluded | An `objdump`-based zero-cross-core-RMW proof (disassemble and grep for `lock`/`cmpxchg`/`xadd`); sanitizer instrumentation injects its own `lock`-prefixed ops, so the check is meaningless under any sanitizer. Verified in Release. |
| `topic_no_quiesce_control` | **added (ASan-only)** | ADR-019 GATE 6 bounded-quiescence firing control: `-DQUARK_TOPIC_NO_QUIESCE` drops the `wait in_flight==0` barrier in `Topic::unsubscribe`, so a churn thread frees an inbox a publisher still references → heap-use-after-free. Wired **only** under ASan, where the freed-memory access reliably traps (`WILL_FAIL` — a non-zero exit *is* the pass). The positive `topic_subscribe_race_test` (barrier present) runs in every config and stays clean. |

### TSan: 153 → 137 (−16)

Three principled groups, all excluded from TSan only:

- **Replace global `operator new` → link collision (11).** TSan's runtime (`libclang_rt.tsan_cxx`)
  defines its own strong global `operator new`/`delete`; a test that also replaces them fails the TSan
  *link* ("multiple definition"). TSan is also not where zero-allocation is measured (the sanitizer
  allocates on its own), so these carry no TSan value. The 0-alloc invariant stays fully verified by
  gcc/clang (and ASan, which intercepts malloc instead of replacing `operator new`):
  `governance_zero_cost_test`, `mailbox_noalloc_test`, `metrics_noalloc_test`, `resource_noalloc_test`,
  `security_secret_zeroize_test`, `security_zero_cost_test`, `serialize_noalloc_test`,
  `stream_noalloc_test`, `tell_noalloc_test`, `timer_noalloc_test`, `topology_zero_cost_test`.
- **Continuously-contended negative controls → TSan pathology (3).** A control that hammers one shared
  location for the whole run makes TSan's race analysis run for minutes (ctest timeout / flaky). The
  *real* (correct) versions of these paths ARE first-class TSan tests and stay TSan-clean; only the
  slow negative controls are not wired as permanent TSan entries — their teeth are demonstrated
  deterministically under gcc/clang/ASan instead:
  `stateless_pool_shared_state_control`, `stream_single_cursor_control`, `stream_reenqueue_control`.
- **Store-buffer / objdump probes that can't manifest under TSan (2).** Same two as the ASan table —
  `stream_close_out_dekker_control` (TSan serializes every atomic and does not model the store buffer,
  so the reorder cannot manifest) and `stream_drain_zero_rmw` (an `objdump` proof; not applicable to any
  instrumented binary) — excluded from **both** ASan and TSan, verified in Release.

The net rule: **the delta is exactly these tests and nothing else.** If a sanitizer run ever reports a
count other than the Release baseline minus its documented exclusions, the difference is a real
regression, not an exclusion — investigate it. (These counts move as tests are added; verify the delta
against a fresh `ctest -N` on each config rather than assuming a fixed number. At `a602f29` that is
211 Release, 194 TSan, and the ASan count follows the exclusion list in `tests/CMakeLists.txt`.)

## Coverage note

The TCP-transport and cluster-relay tests (`tcp_frame_codec_test`, `tcp_transport_loopback_test`,
`tcp_transport_dial_dedup_test`, `tcp_transport_reconnect_test`, `tcp_distribution_integration_test`,
`cluster_dial_dedup_test`) are present and green in **every** config above, including under TSan — the
default TCP transport (010/019/021) is race-free, leak-free, and UB-free over real loopback sockets.

**Impaired-network runs (2026-10-21, bare-metal Ubuntu 26.04, 28 cores, g++ 15.2; Clang 21.1 also
208/208).** `tests/netem/run_matrix.sh` runs the 9 real-socket tests (`tcp_*`, the three
SecureTransport socket tests, `security_rotation_sweep_test`) in a
fresh network namespace per test. It applies `tc netem` to that namespace's private loopback, so the
tests run unchanged and the host's network is untouched. All 9 profiles passed 9/9 in each of 3
runs: baseline, 5 ms±2 ms, 50 ms±10 ms, 1% loss, 5% loss, 25% reordering, 1% duplicate + 0.1%
corrupt, 1 Mbit, and a WAN mix (80 ms±20 ms, 0.5% loss, 2% reorder). Two harness fixes came out of the
first pass. `tcp_transport_control_priority_test` now fails only on a 10 s stall in delivery, not
after a flat 30 s, because its ~8 MiB backlog needs ~67 s on a 1 Mbit link; even when it timed out,
the control frame had arrived **first** (index 0). The fresh namespace per test is needed because
packets left queued in a rate-limited qdisc by one test delayed the next one's connection setup,
which once made `tcp_transport_dial_dedup_test` look like it failed. Limit: Linux TCP retransmits and
reorders below the transport, so these profiles exercise the transport's timing, reconnect and
priority behavior, not frame parsing on a damaged stream.

**Multi-process SWIM.** `tests/netem/run_swim_chaos.sh` (5 processes, one per namespace, iptables
partitions). On 2026-10-21, formation and a 20%-loss flaky link passed, but partition heal and an
asymmetric link cut **failed**: two SWIM defects (021 in `OpenQuestions.md`). After the ADR-049 merge
(2026-10-22) all 5 scenarios end all-alive: an isolated node and a {1,2}|{3,4,5} split each heal about
1 s after the cut is lifted, and with only the 1↔2 link cut nodes 1 and 2 are never suspected
(incarnation stays 1; it was 31). In-process regression tests on a deferred-delivery fabric
(`tests/swim_deferred_fabric.hpp`): `cluster_swim_partition_heal_test` (isolate + split, drop + hold,
8 seeds: worst heal 600 ms; leave → restart → rejoin 120 ms; a crashed node is still dropped in
1.08 s and never resurrected) and `cluster_swim_async_relay_test` (1↔2 cut, 0/10/25 ms per hop:
0 incarnation bumps). Both were run against the old `cluster.hpp` first: the heal test failed
all 32 heal runs and the relay test showed 363 bumps. Full suites after the branch merge (same host):
g++ 15.2 210/210, clang++ 21.1 210/210, ASan+UBSan 209/210, TSan 192/193. After merging
`origin/master` (2026-10-25) the suite is 211-test Release 211/211, TSan 194/194. The two failures are
pre-existing test weaknesses unrelated to SWIM: the `topic_no_quiesce_control` negative control
caught its race in only 1/20 runs, and `tcp_transport_control_priority_test` misses its ordering
threshold under TSan (4/10 on the unmodified code). Both are recorded in `OpenQuestions.md`.

The outbound-streaming-reply suite (`reply_stream_backpressure_test`, `reply_stream_concurrency_test`,
`reply_stream_credit_merge_test`, `reply_stream_exactly_once_test`, `reply_stream_teardown_test`, ADR-018)
and the broadcast suite (`topic_at_most_once_test`, `topic_payload_reclaim_test`,
`topic_publisher_no_stall_test`, `topic_subscribe_race_test`, ADR-019) are likewise present and green in
every config, including TSan — both new fan-out primitives are race-free under real concurrency.

## Known-divergent, asserted-in-part: `type_key` cross-toolchain conformance (008)

`tests/metadata_typekey_conformance_test.cpp` pins **golden `type_key` constants** so any drift —
a new compiler, a new compiler version, or an edit to the name slicer / fingerprint folder — turns
red instead of silently invalidating already-written durable records. (Its sibling
`metadata_typekey_test` asserts only self-consistency and cannot detect drift by construction.)

**Correction (2026-07-30):** this section previously described measured results (specific golden
hex constants, a `g++ 14.3 / clang 20.1` comparison) for a test file that **did not exist in the
repository** — `git log --all` shows it was never committed on any branch; only this section's
prose and `OpenQuestions.md`'s item 1 described it. The test now actually exists and has actually
been run on all three real toolchains this session: **Clang 22.1.5** and real
**MSVC 19.51.36252 (`cl.exe`)** on Windows/x86-64, and real **g++ 15.2.0** via a WSL/Ubuntu
environment reached later the same session (used to run this project's TSan gate, which is not
available on native Windows/clang — running the full suite under TSan is what caught the Tier 2
error described below).

It is deliberately **tiered**, because two of the three tiers do not hold today:

| Tier | Types | Status | Asserted? |
|---|---|---|---|
| 1 | Described types (`fingerprint_v<T>`) | Portable **by construction** — folds `{(tag, wire_type)}`, no name enters it. **Measured byte-identical across all three real toolchains** (`0x60eb68b9763e6f4c`, Clang 22.1.5 / MSVC 19.51 / GCC 15.2.0). | Golden, **all** toolchains |
| 2 | User-defined names, actor protocol folds | **CONVERGED by ADR-061, then made structural for ACTORS by ADR-062 (2026-10-28; see the Tier 4 actor goldens `0xb3d61babaac77327` / `0x32464213a5ce9f09` / `0x92462dbdb69011ea`, identical on all four toolchains)**: ADR-061 result: ONE golden per fixture on every toolchain** — Clang 22 / Clang 21 / g++ 15.2 / real `cl.exe` 14.51 all produce `0xba4a66eb2b3e6ff3` (anonymous namespace; byte-identical to the pre-ADR Clang value), `0x4202adf0189cbfa1` (named namespace), `0x28f38acdf8c467cb` (two-argument template), `0x1a4e0103acebb260` (`quark_type_name`). The compiler-independent normaliser checks run the three spellings directly. **The rest of this row is the historical (pre-ADR-061) measurement:** measured divergent across ALL THREE toolchains, pairwise. Clang `0xba4a66eb2b3e6ff3`, GCC `0xc1a54de107d9d89f`, MSVC `0x2d4c989423ddf201` — all three different. GCC was originally *assumed* to share Clang's golden (both use `__PRETTY_FUNCTION__`-based slicing) and the test originally asserted that; TSan CI running this test on real GCC for the first time failed it, proving that assumption wrong — same slicing logic, different compiler-emitted string, different FNV-1a key. | One golden **per compiler** — **not** `#if !defined(_MSC_VER)`, and **not** a shared Clang/GCC golden (see correction below) |
| 3 | Builtin spellings and templates over them | **Divergent today, confirmed on all three.** `Wrap<size_t>`: Clang `0xb49a2938a7872282`, MSVC `0xd72ff69f238be657`, GCC `0x6a63503e4e5ba5c9` — three different `size_t` spellings (`unsigned long` / `unsigned __int64` / `long unsigned int`), three different folds. | Determinism only — a golden would hard-fail at least two of three compilers |

**Guard correction:** the previous `#if !defined(_MSC_VER)` guard for Tier 2 was wrong and would
have silently mis-asserted on Windows: Clang targeting the MSVC ABI (the default on Windows)
defines **both** `__clang__` and `_MSC_VER`. The subsequent `#if defined(__clang__) ||
defined(__GNUC__)` fix was *also* wrong — it assumed Clang and GCC share one golden, which real
GCC disproved. The test now asserts one golden per compiler, mirroring `metadata.hpp`'s own
per-compiler dispatch order (`__clang__` → `__GNUC__` → `_MSC_VER`) with no branch merging.

Tier 3 is the open half of [OpenQuestions.md](OpenQuestions.md) item 1; closing it (normalizing the
spellings, or mandating explicit per-type keys) promotes Tier 3 into Tier 1. Tier 1's golden is
non-vacuous by negative control: a fields-swapped sibling type (same fields, tags 1↔2 swapped)
moves the fingerprint (`0x54f52ae0703db766`), demonstrating the 016 field-order invariant the fold
rests on. Given Tier 2's GCC-vs-Clang assumption was also wrong once measured, treat every Tier 2
golden as toolchain-*version*-fragile, not just toolchain-*family*-fragile — a future compiler
minor version changing `__PRETTY_FUNCTION__`/`__FUNCSIG__` spelling is exactly the drift this test
exists to catch, not a false alarm to suppress.
