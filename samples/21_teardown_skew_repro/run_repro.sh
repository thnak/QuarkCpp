#!/usr/bin/env bash
# Launches `final_n` real, separate OS processes, each of which exits the INSTANT its own roster
# converges — no shared settle window, deliberately (see main.cpp banner). Reports each process's
# connections_open()-at-exit and wall-clock exit timestamp, then checks whether the readings correlate
# with exit ORDER (later-exiting processes reading fewer connections because earlier siblings already
# tore theirs down) — the signature of the teardown-timing artifact, as opposed to random noise.
#
# Usage: run_repro.sh [final_n=4] [base_port=24000]
set -u -o pipefail

FINAL_N="${1:-4}"
BASE_PORT="${2:-24000}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN=""
for candidate in "$SCRIPT_DIR/../../build-samples/samples/21_teardown_skew_repro" \
                  "$SCRIPT_DIR/../../build/samples/21_teardown_skew_repro"; do
  if [[ -x "$candidate" ]]; then BIN="$candidate"; break; fi
done
if [[ -z "$BIN" ]]; then
  echo "FAIL: binary not built. Build with:" >&2
  echo "  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 21_teardown_skew_repro" >&2
  exit 1
fi

LOGDIR="$(mktemp -d)"
pids=()
for ((i = 1; i <= FINAL_N; i++)); do
  taskset -c 0-3 timeout 20 "$BIN" "$i" "$FINAL_N" "$BASE_PORT" >"$LOGDIR/node_$i.log" 2>&1 &
  pids+=($!)
done
for ((i = 1; i <= FINAL_N; i++)); do wait "${pids[$((i - 1))]}"; done

echo "--- raw per-node results (order launched, NOT order exited) ---"
for ((i = 1; i <= FINAL_N; i++)); do sed "s/^/  /" "$LOGDIR/node_$i.log"; done

echo
echo "--- sorted by actual exit time (wall_clock_ms) ---"
grep -h "wall_clock_ms=" "$LOGDIR"/node_*.log | sort -t= -k4 -n

echo
echo "interpretation: if connections_at_exit DECREASES for processes that exited LATER, that is the"
echo "teardown-timing artifact (each earlier exit closes a socket a later, still-running process was"
echo "still counting) — not a real SWIM/transport churn bug. Compare against 20_swim_membership_stability,"
echo "which proves the same mesh holds N-1 indefinitely when nothing tears down early."

rm -rf "$LOGDIR"
