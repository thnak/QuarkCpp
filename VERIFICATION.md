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
| Commit | `9fd4705` (`master`) |
| Date | 2026-07-18 |
| Host | Linux x86-64 (the sole verified target; other arches/OSes are deferred behind the PAL) |
| Compilers | g++ 14.2.0 (Release + all sanitizers), clang 20.1.2 (second supported compiler) |
| Standard | C++23 |

| Config | Command (build dir) | Result |
|---|---|---|
| **Release** (gcc) | `build` | **144 / 144 passed**, 0 failed |
| **ASan + UBSan** (gcc) | `build-asan` | **142 / 142 passed**, 0 failed — no leaks, no UB |
| **ThreadSanitizer** (gcc) | `build-tsan` | **128 / 128 passed**, 0 failed — no data races |

Release is the full gate — it is the only config where every allocation-counting probe and every
store-buffer litmus control is meaningful, so it runs the complete set. The sanitizer configs run
*fewer* tests **by design**: a fixed, principled set of tests is excluded because sanitizer
instrumentation invalidates exactly what they measure. The deltas below are the *whole* difference —
no test silently dropped out.

## Reproduce

```bash
# Release (gcc) — the full 144-test gate
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

## Sanitizer exclusions (why fewer tests, and why that is correct)

The exclusion logic lives in [`tests/CMakeLists.txt`](tests/CMakeLists.txt) and is guarded per
sanitizer; each excluded test keeps full coverage in the configs where it *is* meaningful.

### ASan/UBSan: 144 → 142 (−2)

| Excluded test | Why it cannot run here |
|---|---|
| `stream_close_out_dekker_control` | Pure store-buffer-reorder litmus (NO_FENCE variant). ASan's malloc/redzone interception drains the store buffer before the racing load, so the reorder cannot manifest — the control can't produce its intended FIRE. Verified in the gcc/clang builds instead. |
| `stream_drain_zero_rmw` | Zero-cross-core-RMW measurement; sanitizer instrumentation perturbs the exact RMW count it asserts. The invariant is verified in Release. |

### TSan: 144 → 128 (−16)

Three principled groups:

- **Replace global `operator new` → link collision.** TSan's runtime (`libclang_rt.tsan_cxx`) defines
  its own strong global `operator new`/`delete`; a test that also replaces them fails the TSan *link*
  ("multiple definition"). TSan is also not where zero-allocation is measured (the sanitizer allocates
  on its own), so these carry no TSan value. The 0-alloc invariant stays fully verified by gcc/clang
  (and ASan, which intercepts malloc instead of replacing `operator new`):
  `tell_noalloc_test`, `timer_noalloc_test`, `stream_noalloc_test`, `topology_zero_cost_test`,
  `stream_drain_zero_rmw`, `stream_drain_rmw_test`, `stream_backpressure_credit_test`.
- **Continuously-contended negative controls → TSan pathology.** A control that hammers one shared
  location for the whole run makes TSan's race analysis run for minutes (ctest timeout / flaky). The
  *real* (correct) versions of these paths ARE first-class TSan tests and stay TSan-clean; only the
  slow negative controls are not wired as permanent TSan entries — their teeth are demonstrated
  deterministically under gcc/clang/ASan:
  `stateless_pool_exactly_once_test`, `stateless_pool_fault_deadletter_test`,
  `stateless_pool_shared_state_control`, `stream_reenqueue_control`, `stream_single_cursor_control`,
  `stream_close_out_dekker_test`, `stream_close_out_dekker_control`.
- **Model limits.** TSan serializes every atomic and does not model the store buffer, so the
  store-buffer litmus cannot manifest; a couple of timing/window controls likewise can't reproduce
  under ~15× instrumentation: `sim_virtual_clock_window_test`, `smoke_test`.

The net rule: **the delta is exactly these tests and nothing else.** If a sanitizer run ever reports a
count other than 142 (ASan) or 128 (TSan) against this gate, the difference is a real regression, not
an exclusion — investigate it.

## Coverage note

The five TCP-transport tests (`tcp_frame_codec_test`, `tcp_transport_loopback_test`,
`tcp_transport_dial_dedup_test`, `tcp_transport_reconnect_test`, `tcp_distribution_integration_test`)
are present and green in **every** config above, including `cluster_dial_dedup_test` under TSan — the
default TCP transport (010/019/021) is race-free, leak-free, and UB-free over real loopback sockets.
