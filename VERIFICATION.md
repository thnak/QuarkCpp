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
| Commit | `7dc7e81` (`feat/broadcast-topic`) |
| Date | 2026-07-19 |
| Host | Linux x86-64 (the sole verified target; other arches/OSes are deferred behind the PAL) |
| Compilers | g++ 14.2.0 (Release + all sanitizers), clang 20.1.2 (second supported compiler) |
| Standard | C++23 |

| Config | Command (build dir) | Result |
|---|---|---|
| **Release** (gcc) | `build` | **153 / 153 passed**, 0 failed |
| **ASan + UBSan** (gcc) | `build-asan` | **152 / 152 passed**, 0 failed — no leaks, no UB |
| **ThreadSanitizer** (gcc) | `build-tsan` | **137 / 137 passed**, 0 failed — no data races |

Release is the full gate — it is the only config where every allocation-counting probe and every
store-buffer litmus control is meaningful, so it runs the complete set. The sanitizer configs run a
*different-sized* set **by design**: a fixed, principled set of tests is excluded because sanitizer
instrumentation invalidates exactly what they measure, and one sanitizer-only firing control
(`topic_no_quiesce_control`) is *added* under ASan because it needs ASan's trap to be observable. The
deltas below are the *whole* difference — no test silently dropped out.

## Reproduce

```bash
# Release (gcc) — the full 153-test gate
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
count other than 152 (ASan) or 137 (TSan) against a 153-test Release baseline, the difference is a real
regression, not an exclusion — investigate it. (These counts move as tests are added; verify the delta
against a fresh `ctest -N` on each config rather than assuming a fixed number.)

## Coverage note

The TCP-transport and cluster-relay tests (`tcp_frame_codec_test`, `tcp_transport_loopback_test`,
`tcp_transport_dial_dedup_test`, `tcp_transport_reconnect_test`, `tcp_distribution_integration_test`,
`cluster_dial_dedup_test`) are present and green in **every** config above, including under TSan — the
default TCP transport (010/019/021) is race-free, leak-free, and UB-free over real loopback sockets.

The outbound-streaming-reply suite (`reply_stream_backpressure_test`, `reply_stream_concurrency_test`,
`reply_stream_credit_merge_test`, `reply_stream_exactly_once_test`, `reply_stream_teardown_test`, ADR-018)
and the broadcast suite (`topic_at_most_once_test`, `topic_payload_reclaim_test`,
`topic_publisher_no_stall_test`, `topic_subscribe_race_test`, ADR-019) are likewise present and green in
every config, including TSan — both new fan-out primitives are race-free under real concurrency.
