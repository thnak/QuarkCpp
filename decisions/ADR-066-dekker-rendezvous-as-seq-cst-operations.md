# ADR-066 — Dekker rendezvous as seq_cst operations, not standalone fences (001/002/003/015/024)

Status: **Accepted** (2026-10-28). Amends the *spelling* chosen by ADR-004 (store + `seq_cst` fence +
load); keeps every rendezvous ADR-002/003/004/010/014/015/028/031/032 proved, and its CI guard.
Implemented in `exec_state.hpp`, `mailbox.hpp`, `run_queue.hpp`, `activation.hpp`, `engine.hpp`,
`stream_activation.hpp`, `stream_channel.hpp`; `pal::store_load_barrier()` is removed.

## Problem
Every StoreLoad (Dekker) rendezvous was spelled `x.store(release); atomic_thread_fence(seq_cst);
y.load(acquire)`, with the producer half's fence elided on x86 only. ThreadSanitizer does not model
standalone fences, so GCC 14 emits `-Wtsan` ("atomic_thread_fence is not supported with
-fsanitize=thread") ~2300 times per TSan build, and the TSan runs cannot see the edges the
correctness argument relies on (ADR-028/033/038 each recorded this as a proof-method caveat). On
AArch64 the form costs a `dmb ish` on the consumer **and** one per enqueue/wake on the producer.

## Decision
Spell each rendezvous as `seq_cst` **operations** on both halves: the store (or RMW) and the
following probe load are both `seq_cst`, so all four accesses sit in the single total order S and
"both sides miss" is forbidden by C++20 [atomics.order] itself.

| Rendezvous | Store half (was → now) | Load half (was → now) |
|---|---|---|
| Mailbox close-out (Idle / Dormant / reentrant Parked) | `exec.store(rel)`+fence → `release_to_idle_sc` / `retire_to_dormant` / `park_sc` (seq_cst store) | `tail_.load(acq)` → `probe_has_work` seq_cst |
| ↳ producer side | `tail_.exchange(acq_rel)`+fence(non-x86) → `exchange(seq_cst)` | `notify_enqueued` / `readmit_from_*` CAS acq_rel/acq → seq_cst/seq_cst |
| Run-queue wake (engine `park`) | `idle_mask_.fetch_or(acq_rel)`+fence → `fetch_or(seq_cst)` | `any_work` → `has_work` seq_cst |
| ↳ producer side | run-queue `exchange(acq_rel)`+fence(non-x86) → `exchange(seq_cst)` | `wake_one` `idle_mask_.load` acq → seq_cst |
| Drain-owner close-out / backstop / cooperative evict | `drain_owner.store(rel)`+fence → `store(seq_cst)` | `run_queue.has_work` seq_cst |
| Stream arm close-out (024) | `armed_.store(rel)`+fence → `store(seq_cst)` | `occupancy_seq_cst()` (head seq_cst) |
| ↳ producer side | `armed_.exchange(acq_rel)` → `exchange(seq_cst)` | (the exchange's read) |
| Stream un-stall (reverse Dekker) | consumer: fence → `tail_.store_seq_cst(tail)` (one republish per batch); producer: `stalled_.store(rel)`+fence → `store(seq_cst)` | consumer: `stalled_.exchange(seq_cst)`; producer: `credit_available_seq_cst()` |
| Watchdog / request_stop | `lane_actions.fetch_or(acq_rel)`+fence → `fetch_or(seq_cst)` | `readmit_from_parked` seq_cst |

Two fences were **removed outright** because they ordered nothing the protocol reads:
`Activation::complete_one` (the lane reads `ready` under the same mutex the carrier pushed under —
the mutex totally orders the two critical sections) and `StreamChannel::notify_departure` (the
departure side does no load after clearing `keep_going` except the `acq_rel` `credit_gen_` RMW, and
the producer captured `g` before re-checking `keep_going`: either `g` predates the bump and the
wait/notify pair wakes it, or `g` read the bump and synchronizes-with it, so the re-check sees the
clear). The stub re-arm exchanges (consumer side, not a Dekker half) stay `acq_rel`.

## Codegen (clang 22 `-O2`, a stand-alone TU reproducing the exact access shapes)
| Target | consumer close-out | producer enqueue → wake CAS |
|---|---|---|
| x86-64 Linux (`__atomic` builtins) | `mov` + `lock or [rsp]` + `mov` → **`xchg` + `mov`** | `xchg` + `lock cmpxchg` (unchanged) |
| x86-64 Windows (clang + MSVC STL) | `mov` + `lock inc [rsp]` + `mov` → **`xchg` + `mov`** | unchanged |
| AArch64 (v8.0 / v8.1 LSE) | `stlrb` + **`dmb ish`** + `ldar` → `stlrb` + `ldar` | `swpal` + **`dmb ish`** + `casalb` → `swpal` + `casalb` |

x86: one locked instruction replaces one locked instruction (on the line the consumer was already
writing — no new coherence traffic). AArch64: every `dmb ish` on these paths is gone, including the
per-enqueue one ADR-004 accepted to "keep the exchange acq_rel" — on ARMv8 an `acq_rel` and a
`seq_cst` exchange are the same instruction, so that trade bought nothing there.

**Hot-path rules.** No new C++ RMW on any drain path: the consumer-side changes are *stores* and
*loads*; the per-message dequeue loop, the per-frame stream cursors (`MonotoneCursor` stays
load/store-only) and the 024 objdump gate (`stream_drain_zero_rmw`) are untouched. The close-out
`xchg` that x86 uses to lower a `seq_cst` store runs once per empty-transition exactly where the
fence's locked instruction ran before. Descriptor size untouched.

## Evidence (this box: Windows 11, clang 22.1 Release, x86-64)
- `ctest -j4`: 244/244 pass. Litmus tests now mirror the shipped spelling (seq_cst store + seq_cst
  load): `activation_deactivate_close_out_dekker_test` and `stream_close_out_dekker_test` **0 lost /
  50M**; their controls (same pair, relaxed) still **fire**: 146,148 / 162,439 lost per 50M.
  `sched_no_lost_wakeup_test` 0 stalled / 200k; its broken-park control still strands. The un-stall /
  departure paths are covered by the existing `fanout_block_test`, `fanout_subscribe_race_test`,
  `stream_*` and `reply_stream_*` tests (all pass).
- Benches, original vs ADR-066 binaries interleaved, medians of 5-12 runs, single-thread ones
  pinned to one core: `activation_bench` activate/deactivate cycle 28.80 → 28.53 M/s (ranges
  overlap), `sched_bench` full lifecycle 21.5 → 21.4 M msg/s, `mailbox_bench` peak 100.7 → 100.5 M
  msg/s, `stream_bench` 382 → 378 M frames/s, `fanout_gap_bench` pop 86.8 → 87.1 ns. All within run
  noise — the expected result for "one locked instruction for one locked instruction" on x86.
  Multi-thread benches (ping-pong, e2e engine, wakeup) are bimodal run-to-run on this host and showed
  no consistent direction over 12 reps.

## Limits / not verified here
- GCC and MSVC were not available locally. GCC lowers a `seq_cst` store on x86 either to `xchg` or
  to `mov` + `mfence` depending on version/tuning — never more than the fence form it replaces. The
  `-Wtsan` count reaching zero needs the GCC TSan CI leg to confirm (no `atomic_thread_fence` remains
  in `include/`, `pal/`, `src/`, `tests/`, `bench/`).
- AArch64 codegen above is from clang cross-compilation, not executed here; CI's arm64 legs run the
  suites. A tool-checked (herd7/GenMC) weak-memory re-gate remains open, as before.
- The stream **arm** rendezvous keeps its pre-existing reliance on hardware: the producer's per-frame
  `head` store is a `release` store (it must stay a plain store), so its ordering before the
  `seq_cst` arm exchange is a property of x86-TSO / ARMv8 (a store followed by an RMW with acquire
  semantics), not of the C++ model — unchanged by this ADR, and still noted in 024.
