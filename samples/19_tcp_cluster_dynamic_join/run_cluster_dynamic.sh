#!/usr/bin/env bash
# Starts `base_n` real, separate OS processes that form a cluster and run for a while, THEN starts
# `final_n - base_n` more processes that join that ALREADY-RUNNING cluster live (no restart of the
# base nodes) — the scenario 18_tcp_cluster_multiprocess cannot demonstrate, since every one of its
# processes must be told the full roster before any of them starts.
#
# All processes are actually launched at the same wall-clock moment (so they share one settle-window
# deadline, see main.cpp) — the "late" nodes just sleep internally for join_delay_ms before touching
# anything, which is indistinguishable from actually starting them later.
#
# Usage: run_cluster_dynamic.sh [base_n=3] [final_n=4] [base_port=22000] [join_delay_ms=4000]
set -u -o pipefail

BASE_N="${1:-3}"
FINAL_N="${2:-4}"
BASE_PORT="${3:-22000}"
JOIN_DELAY_MS="${4:-4000}"
SETTLE_BUDGET_MS=$(( JOIN_DELAY_MS + 16000 ))

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN=""
for candidate in "$SCRIPT_DIR/../../build-samples/samples/19_tcp_cluster_dynamic_join" \
                  "$SCRIPT_DIR/../../build/samples/19_tcp_cluster_dynamic_join"; do
  if [[ -x "$candidate" ]]; then BIN="$candidate"; break; fi
done
if [[ -z "$BIN" ]]; then
  echo "FAIL: binary not built. Build with:" >&2
  echo "  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 19_tcp_cluster_dynamic_join" >&2
  exit 1
fi

if (( FINAL_N <= BASE_N )); then
  echo "FAIL: final_n ($FINAL_N) must be > base_n ($BASE_N)" >&2
  exit 1
fi

LOGDIR="$(mktemp -d)"
echo "base cluster: nodes 1..$BASE_N start immediately"
echo "joining live: nodes $((BASE_N + 1))..$FINAL_N join after ${JOIN_DELAY_MS}ms (base cluster is already up and serving)"
echo "logs in $LOGDIR"

pids=()
# The base cluster: node 1 has no seed (bootstraps alone); nodes 2..base_n join through node 1.
for ((i = 1; i <= BASE_N; i++)); do
  seed=0
  (( i > 1 )) && seed=1
  taskset -c 0-3 timeout 40 "$BIN" "$i" "$seed" "$FINAL_N" "$BASE_PORT" 0 "$SETTLE_BUDGET_MS" \
    >"$LOGDIR/node_$i.log" 2>&1 &
  pids+=($!)
done

# The late joiner(s): each also joins through node 1, but sleeps internally first — the base cluster
# is fully up, ticking, and (per main.cpp) already sending/receiving traffic among itself by the time
# any of these actually starts its own bind/join.
delay="$JOIN_DELAY_MS"
for ((i = BASE_N + 1; i <= FINAL_N; i++)); do
  taskset -c 0-3 timeout 40 "$BIN" "$i" 1 "$FINAL_N" "$BASE_PORT" "$delay" "$SETTLE_BUDGET_MS" \
    >"$LOGDIR/node_$i.log" 2>&1 &
  pids+=($!)
  delay=$(( delay + 1000 ))  # stagger multiple late joiners so they don't all race in at once
done

ok=1
for ((i = 1; i <= FINAL_N; i++)); do
  idx=$((i - 1))
  if wait "${pids[$idx]}"; then
    rc=0
  else
    rc=$?
  fi
  tag="base"
  (( i > BASE_N )) && tag="LATE JOINER"
  echo "--- node $i [$tag] (pid ${pids[$idx]}, exit=$rc) ---"
  sed "s/^/  /" "$LOGDIR/node_$i.log"
  if [[ $rc -ne 0 ]]; then
    ok=0
    if [[ $rc -eq 124 ]]; then
      echo "  ^ killed by timeout — treat as a real hang, not the documented at-most-once race"
    fi
  fi
done

echo
if [[ $ok -eq 1 ]]; then
  echo "CLUSTER OK — $((FINAL_N - BASE_N)) node(s) joined a running $BASE_N-node cluster live, no restart"
  rm -rf "$LOGDIR"
  exit 0
else
  echo "CLUSTER FAIL — full logs kept in $LOGDIR"
  exit 1
fi
