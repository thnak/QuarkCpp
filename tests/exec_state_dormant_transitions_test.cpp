// Tests ADR-028 Phase 1 §Dormant ExecState — pure ExecStateCell unit coverage for the new
// Running<->Dormant<->Scheduled edges, mirroring the existing Parked edges byte-for-byte:
//   retire_to_dormant()      : Running -> Dormant   (release store, mirrors park())
//   reacquire_from_dormant() : Dormant -> Running   (mirrors reacquire_from_idle/_parked — the
//                              close-out abort-eviction race: a message won, keep draining)
//   readmit_from_dormant()   : Dormant -> Scheduled (mirrors readmit_from_parked — a future
//                              reactivation path re-admitting an evicted activation, ADR-028 Phase 4)
// Single-threaded, deterministic — no race here; the concurrent [Deactivate,M] proof is the
// separate activation_deactivate_close_out_dekker_test.cpp isolating Dekker litmus.
#include <cstdio>

#include "quark/core/exec_state.hpp"

using namespace quark;

namespace {
void check(bool cond, const char* what, bool& ok) {
    if (!cond) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
}  // namespace

int main() {
    bool ok = true;

    // Still lock-free with the new enumerator (exec_state.hpp already static_asserts this at
    // namespace scope; re-check here so a violation fails THIS test's compile too).
    static_assert(std::atomic<ExecState>::is_always_lock_free);

    // --- Happy path: Idle -> Scheduled -> Running -> Dormant -> Running (abort) -> Dormant ->
    //                 Scheduled (readmit) ------------------------------------------------------
    {
        ExecStateCell cell;
        check(cell.state() == ExecState::Idle, "starts Idle", ok);

        check(cell.notify_enqueued(), "Idle->Scheduled wakes", ok);
        check(cell.state() == ExecState::Scheduled, "now Scheduled", ok);

        check(cell.try_acquire(), "Scheduled->Running", ok);
        check(cell.state() == ExecState::Running, "now Running", ok);

        cell.retire_to_dormant();
        check(cell.state() == ExecState::Dormant, "Running->Dormant (retire)", ok);

        // Abort-eviction race: a message raced in before the retire committed.
        check(cell.reacquire_from_dormant(), "Dormant->Running (abort eviction)", ok);
        check(cell.state() == ExecState::Running, "back to Running after abort", ok);

        // Retire again, this time let a (future) reactivation path readmit it.
        cell.retire_to_dormant();
        check(cell.state() == ExecState::Dormant, "Running->Dormant again", ok);
        check(cell.readmit_from_dormant(), "Dormant->Scheduled (readmit)", ok);
        check(cell.state() == ExecState::Scheduled, "Scheduled after readmit", ok);
    }

    // --- Negative: the Dormant edges must fail (no-op) from any state other than Dormant ------
    {
        ExecStateCell cell;  // starts Idle
        check(!cell.reacquire_from_dormant(), "reacquire_from_dormant fails from Idle", ok);
        check(cell.state() == ExecState::Idle, "state unchanged after failed reacquire", ok);
        check(!cell.readmit_from_dormant(), "readmit_from_dormant fails from Idle", ok);
        check(cell.state() == ExecState::Idle, "state unchanged after failed readmit", ok);
    }
    {
        ExecStateCell cell;
        check(cell.notify_enqueued() && cell.try_acquire(), "reach Running", ok);
        check(!cell.reacquire_from_dormant(), "reacquire_from_dormant fails from Running", ok);
        check(cell.state() == ExecState::Running, "still Running", ok);
        check(!cell.readmit_from_dormant(), "readmit_from_dormant fails from Running", ok);
        check(cell.state() == ExecState::Running, "still Running (readmit no-op)", ok);
    }

    // --- ADR-077: notify_enqueued_fast() keeps every transition, and returns false (no locked RMW)
    // from states whose current owner will drain/re-admit. The concurrent memory-order half is the
    // separate exec_state_wake_load_dekker_test.
    {
        ExecStateCell cell;
        check(cell.notify_enqueued_fast(), "fast: Idle wakes", ok);
        check(cell.state() == ExecState::Scheduled, "fast: Idle->Scheduled", ok);
        check(!cell.notify_enqueued_fast(), "fast: already Scheduled does not wake", ok);
        check(cell.try_acquire(), "reach Running", ok);
        check(!cell.notify_enqueued_fast(), "fast: Running does not wake", ok);
        check(cell.state() == ExecState::Running, "fast: Running unchanged", ok);
    }
    {
        ExecStateCell cell;
        check(cell.notify_enqueued() && cell.try_acquire(), "reach Running", ok);
        cell.park();
        check(!cell.notify_enqueued_fast(), "fast: Parked does not wake (completion re-admits)", ok);
        check(cell.state() == ExecState::Parked, "fast: Parked unchanged", ok);
    }
    {
        ExecStateCell cell;
        check(cell.notify_enqueued() && cell.try_acquire(), "reach Running", ok);
        cell.retire_to_dormant();
        check(cell.notify_enqueued_fast(), "fast: Dormant re-admits", ok);
        check(cell.state() == ExecState::Scheduled, "fast: Dormant->Scheduled", ok);
    }
    {
        ExecStateCell cell;
        check(cell.notify_enqueued() && cell.try_acquire(), "reach Running", ok);
        cell.retire_to_dormant();
        check(cell.retire_from_dormant(), "reach Retired", ok);
        check(!cell.notify_enqueued_fast(), "fast: Retired does not wake (sweep rescues)", ok);
        check(cell.state() == ExecState::Retired, "fast: Retired unchanged", ok);
    }

    std::printf("exec_state_dormant_transitions_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
