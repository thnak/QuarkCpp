# ADR-077 — Elide the producer wake CAS with a seq_cst load fast path (001/002/003/015)

Status: **Proposed.** Implements the highest-self-cost producer path (`ExecStateCell::notify_enqueued`,
measured **17.8% self** on the 14-lane stress soak). Companion pool work: ADR-078.

## Problem

Every `tell`/`post` ends in `wake_after_enqueue()`:

```cpp
bool wake = exec_.notify_enqueued();              // seq_cst CAS Idle -> Scheduled
if (!wake && exec_.state() == ExecState::Dormant) // second load, every failed CAS
    wake = exec_.readmit_from_dormant();
```

The CAS is ADR-066's producer half of the Dekker rendezvous, so it must stay when the actor really is
`Idle`. But under sustained load the CAS **almost always fails**: the actor is `Running` (the current
drain or its ADR-036 linger will observe the message) or already `Scheduled`. A failed `lock cmpxchg` is
still a locked instruction, and the follow-up `state()` load is a second access. That is pure overhead on
the common path.

## Decision

Add `ExecStateCell::notify_enqueued_fast()` and use it from `Activation::wake_after_enqueue()`:

```cpp
const ExecState st = s_.load(std::memory_order_seq_cst);   // the no-transition half of the rendezvous
if (st == ExecState::Idle) {
    if (cas_idle_to_scheduled()) return true;              // the existing seq_cst CAS
    if (s_.load(std::memory_order_seq_cst) == ExecState::Dormant)
        return readmit_from_dormant();                      // the load/CAS eviction race window
    return false;
}
if (st == ExecState::Dormant) return readmit_from_dormant();
return false;  // Scheduled / Running / Parked / Retired: the owner drains or re-admits
```

The load is **seq_cst, never acquire/relaxed**. `notify_enqueued()` (the pure CAS) remains for
`stream_activation`, the simulator, and tests until each is audited separately.

### Why a seq_cst load is sound here

Take the producer's `mailbox_.enqueue` (a seq_cst exchange, ADR-066) then this seq_cst load; and the
consumer's `release_to_idle_sc()` (seq_cst store) then its seq_cst `probe_has_work()` load. All four
operations are in the single total order S. If the producer's state load reads a non-Idle state, that
read precedes the consumer's Idle store in S (the store has not happened yet from the producer's view);
the enqueue precedes the producer's load in program order; the store precedes the probe in program order.
Therefore `enqueue < load < store < probe` in S, and the probe must observe the enqueued message — so
"producer reads non-Idle AND consumer probes empty" is impossible. That is the same Dekker argument
ADR-066 uses for the failed-CAS read, with a load instead of an RMW.

A transition is needed only when the producer observes `Idle`; that path keeps the seq_cst CAS. If the
actor is evicted to `Dormant` between the load and the CAS, the CAS fails and the explicit re-check
performs the `Dormant -> Scheduled` readmit, preserving the old CAS-only window exactly.

`Parked` and `Retired` are unchanged: the CAS never admitted them before, and the completion/sweep paths
remain the wakers.

## Evidence

- New litmus `tests/exec_state_wake_load_dekker_test.cpp` (same methodology as
  `activation_deactivate_close_out_dekker_test.cpp`): producer seq_cst enqueue + seq_cst state load vs
  consumer seq_cst Idle store + seq_cst mailbox probe. Positive: **0 lost / 50M**. Control
  (`-DQUARK_EXEC_WAKE_LOAD_NO_FENCE`, relaxed pair): **lost > 0** (the ordering is load-bearing).
- Deterministic transition coverage added to `exec_state_dormant_transitions_test`
  (Idle wakes; Scheduled/Running/Parked/Retired do not; Dormant re-admits).
- Existing controls: `sched_no_lost_wakeup_test` (0 stalled / 200k; broken-park control strands),
  activation/stream close-out Dekker tests, full Release suite, focused ASan+UBSan, TSan on the
  scheduler/engine subset. 14-lane 60 s soak with zero loss/dup.
- A/B on the 28-core Linux host, 14 pairs, 300M-message work quantum, 5 interleaved reps each
  (`build/quark_stress_bench` baseline vs the perf build):

| metric | baseline (median) | ADR-077/078 | delta |
|---|---|---|---|
| aggregate throughput | 25.19 M msg/s (24.53–25.71) | **26.80 M msg/s (25.74–26.88)** | **+6.4%** |
| enqueue p50 | 280 ns | **212 ns** | **−24%** |
| enqueue p99 | 9,143 ns | 9,535 ns | +4.3% |
| enqueue p999 | 21,059 ns | 21,856 ns | +3.8% |
| delivered | 300M, zero loss/dup | 300M, zero loss/dup | — |

  `mailbox_pool_partition_bench` (same host): presized P1 7.03→7.67, P2 6.97→7.36, P3 7.30→7.48 M msg/s;
  undersized P1 5.31→5.83, P2 5.54→6.20, P3 5.18→5.78. The 60 s / 2B-message soak is run separately for
  stability (see VERIFICATION).

## Limits

- The litmus control is x86-64 only (same reason as the other store-buffer litmus controls); the
  positive test runs everywhere.
- `stream_activation` still uses the pure CAS; it is a separate hot-path audit (it may be worth the same
  fast path, but its Dormant/stream semantics were not part of this proof).
- If the A/B does not show a win, the change is reverted (the pool fast path is independent).
