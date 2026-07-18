#!/usr/bin/env bash
# 023 performance REGRESSION GATE — the pass/fail wiring 023 §"Regression gating" calls for.
#
# Runs every built microbenchmark and grades the verdict tokens the bench harness prints
# (bench/bench_harness.hpp: lat_verdict / thr_verdict) against the 023 budget table:
#
#   [MISS]           -> a HARD budget was violated (latency over the hard ceiling, or throughput
#                       under the hard floor). This is a VETO: the gate FAILS (exit 1). 023 §"The
#                       budgets": "a design that misses [a Hard budget] is rejected."
#   [hard] / [floor] -> a GOAL was missed but the hard bound held (within ceiling / above floor).
#                       This is a tracked regression, NOT a veto: the gate WARNS and stays green.
#   [goal]           -> met the aspiration. Silent pass.
#
# A bench that crashes or exits non-zero is treated as a HARD failure (a perf bench that cannot run
# is a broken gate). Custom informational tokens some benches print ([flat], [within noise],
# [under ceiling], …) are neither a miss nor a goal-warn and are ignored by design.
#
# MACHINE SAFETY (see bench/README.md + repo rule): benches are pinned to a bounded core set via
# taskset (never all cores — this dev box can hang/power off under full saturation). On a CI runner
# without taskset the benches still run (unpinned); ns-scale numbers are only trustworthy on the
# pinned, quiesced 023 reference machine, so this gate's JOB IS THE HARD VETO — goal warnings on a
# noisy shared runner are informational, exactly as 023 §"Regression gating" prescribes.
#
# Usage:  ci_bench_gate.sh [BENCH_DIR]
#   BENCH_DIR defaults to <repo>/build/bench (relative to this script), then ./build/bench.
set -uo pipefail

# --- Resolve the directory holding the *_bench executables -------------------------------------
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bench_dir="${1:-}"
if [[ -z "$bench_dir" ]]; then
  for cand in "$script_dir/../build/bench" "$PWD/build/bench" "$PWD"; do
    if compgen -G "$cand/*_bench" >/dev/null 2>&1; then bench_dir="$cand"; break; fi
  done
fi
if [[ -z "$bench_dir" ]] || ! compgen -G "$bench_dir/*_bench" >/dev/null 2>&1; then
  echo "bench-gate: no *_bench executables found (looked in '${bench_dir:-<none>}'). Build the bench" >&2
  echo "            targets first: cmake --build <dir> --target <name>_bench, or 'cmake --build <dir>'." >&2
  exit 2
fi
bench_dir="$(cd "$bench_dir" && pwd)"

# --- Core pin (bounded; never all cores) -------------------------------------------------------
# Pin to at most 4 cores (repo machine-safety cap), but never request cores the box does not have:
# `taskset -c 0-3` on a 2-CPU runner fails outright, which would look like every bench crashing. Cap
# the top core index at min(nproc, 4) - 1.
pin=()
if command -v taskset >/dev/null 2>&1; then
  ncpu=1
  if command -v nproc >/dev/null 2>&1; then ncpu="$(nproc)"; fi
  cap=4
  (( ncpu < cap )) && cap=$ncpu
  top=$(( cap - 1 ))
  pin=(taskset -c "0-${top}")
  echo "bench-gate: pinning benches to cores 0-${top} (taskset; ${ncpu} CPU(s) present, ≤4 cap)"
else
  echo "bench-gate: taskset unavailable — running unpinned (numbers are veto-only, not a reference stamp)"
fi

echo "bench-gate: 023 performance regression gate over $bench_dir"
echo "==============================================================================="

hard_fail=0
goal_warn=0
ran=0
hard_report=""
warn_report=""

for exe in "$bench_dir"/*_bench; do
  [[ -x "$exe" ]] || continue
  # The supervision no-guard build is a CONTROL for the guard-ratio comparison, not a budgeted
  # bench of its own — skip it from the gate (it prints no verdict tokens).
  name="$(basename "$exe")"
  [[ "$name" == "supervision_bench_noguard" ]] && continue

  out="$("${pin[@]}" "$exe" 2>&1)"
  rc=$?
  ran=$((ran + 1))

  if [[ $rc -ne 0 ]]; then
    hard_fail=$((hard_fail + 1))
    hard_report+="  [CRASH] ${name} exited ${rc}"$'\n'
    echo "### ${name}: DID NOT RUN (exit ${rc}) — treated as HARD failure"
    printf '%s\n' "$out" | sed 's/^/    /'
    continue
  fi

  misses="$(printf '%s\n' "$out" | grep -F '[MISS]' || true)"
  warns="$(printf '%s\n'  "$out" | grep -E '\[hard\]|\[floor\]' || true)"

  if [[ -n "$misses" ]]; then
    n=$(printf '%s\n' "$misses" | grep -c .)
    hard_fail=$((hard_fail + n))
    while IFS= read -r line; do
      [[ -n "$line" ]] && hard_report+="  [MISS] ${name}: ${line#"${line%%[![:space:]]*}"}"$'\n'
    done <<< "$misses"
  fi
  if [[ -n "$warns" ]]; then
    n=$(printf '%s\n' "$warns" | grep -c .)
    goal_warn=$((goal_warn + n))
    while IFS= read -r line; do
      [[ -n "$line" ]] && warn_report+="  [goal] ${name}: ${line#"${line%%[![:space:]]*}"}"$'\n'
    done <<< "$warns"
  fi

  status="ok"
  [[ -n "$warns"  ]] && status="goal-regression"
  [[ -n "$misses" ]] && status="HARD-VIOLATION"
  echo "### ${name}: ${status}"
done

echo "==============================================================================="
echo "bench-gate summary: ${ran} benches run, ${hard_fail} hard violation(s), ${goal_warn} goal regression(s)"

if [[ $goal_warn -gt 0 ]]; then
  echo
  echo "GOAL REGRESSIONS (tracked, not a veto — within hard ceiling / above hard floor):"
  printf '%s' "$warn_report"
fi

if [[ $hard_fail -gt 0 ]]; then
  echo
  echo "HARD BUDGET VIOLATIONS (veto — 023 rejects a design that misses a Hard budget):"
  printf '%s' "$hard_report"
  echo
  echo "bench-gate: FAIL"
  exit 1
fi

echo "bench-gate: PASS (no hard-budget violations)"
exit 0
