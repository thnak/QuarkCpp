// Shared driving harness for the 015 reentrancy/quiescence tests. Reproduces the engine's
// `run_activation` loop (002) standalone so a test can drive one Activation's lane directly (no full
// Engine): acquire Scheduled→Running, drain, close out to Idle, or return when the lane parks on
// in-flight async frames. Carrier completion is driven by `Activation::complete_one()` (015).
#pragma once

#include <cstdint>
#include <cstdio>

#include "quark/core/activation.hpp"

namespace quark::test {

enum class DriveEnd : std::uint8_t {
    Idle,    // DrainedEmpty → close_out relinquished to Idle (nothing in flight)
    Parked,  // Suspended → in-flight frames remain; a carrier must re-admit (complete_one)
    NotOwned,  // not schedulable right now (another owner / not Scheduled / sealed-and-parked)
};

// Drive one activation lane to a stopping point (Idle or Parked). Budget is set high so tests are
// not perturbed by the fairness yield; the Busy spin is bounded so a stuck producer cannot hang.
inline DriveEnd drive(Activation& act, std::uint32_t budget = 1u << 20) {
    if (!act.try_acquire()) return DriveEnd::NotOwned;
    std::uint64_t busy = 0;
    for (;;) {
        switch (act.drain_step(budget)) {
            case Activation::DrainOutcome::DrainedEmpty:
                if (act.close_out()) {
                    busy = 0;
                    continue;
                }
                return DriveEnd::Idle;
            case Activation::DrainOutcome::Busy:
                if (++busy > (1u << 24)) return DriveEnd::Parked;  // give up (should not happen)
                continue;
            case Activation::DrainOutcome::BudgetExhausted:
                act.yield_to_scheduled();
                if (act.try_acquire()) {
                    busy = 0;
                    continue;
                }
                return DriveEnd::NotOwned;
            case Activation::DrainOutcome::Suspended:
                return DriveEnd::Parked;
        }
    }
}

inline void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace quark::test
