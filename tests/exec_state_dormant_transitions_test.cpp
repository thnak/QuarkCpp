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

    std::printf("exec_state_dormant_transitions_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
