#!/usr/bin/env bash
# Launches N real, separate OS processes of 18_tcp_cluster_multiprocess so "the cluster" is actually
# N independent address spaces talking only over 127.0.0.1 sockets, then aggregates their individual
# PASS/FAIL. Every process is pinned to the same <=4 cores (machine-safety rule: never saturate the
# box), and every process has its own internal bounded wait — so a real hang shows up as this script's
# own `timeout` killing that one process (exit 124 below), not this script hanging.
#
# Usage: run_cluster.sh [num_nodes=4] [messages_per_lane=150] [base_port=21000]
set -u -o pipefail

NUM_NODES="${1:-4}"
MSGS="${2:-150}"
BASE_PORT="${3:-21000}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN=""
for candidate in "$SCRIPT_DIR/../../build-samples/samples/18_tcp_cluster_multiprocess" \
                  "$SCRIPT_DIR/../../build/samples/18_tcp_cluster_multiprocess"; do
  if [[ -x "$candidate" ]]; then BIN="$candidate"; break; fi
done
if [[ -z "$BIN" ]]; then
  echo "FAIL: binary not built. Build with:" >&2
  echo "  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 18_tcp_cluster_multiprocess" >&2
  exit 1
fi

LOGDIR="$(mktemp -d)"
echo "launching $NUM_NODES separate processes (logs in $LOGDIR) ..."

pids=()
for ((i = 1; i <= NUM_NODES; i++)); do
  taskset -c 0-3 timeout 30 "$BIN" "$i" "$NUM_NODES" "$MSGS" "$BASE_PORT" >"$LOGDIR/node_$i.log" 2>&1 &
  pids+=($!)
done

ok=1
for ((i = 1; i <= NUM_NODES; i++)); do
  idx=$((i - 1))
  if wait "${pids[$idx]}"; then
    rc=0
  else
    rc=$?
  fi
  echo "--- node $i (pid ${pids[$idx]}, exit=$rc) ---"
  sed "s/^/  /" "$LOGDIR/node_$i.log"
  if [[ $rc -ne 0 ]]; then
    ok=0
    if [[ $rc -eq 124 ]]; then
      echo "  ^ killed by timeout — treat as a real hang/deadlock, not the documented at-most-once race"
    fi
  fi
done

echo
if [[ $ok -eq 1 ]]; then
  echo "CLUSTER OK ($NUM_NODES separate processes, base_port=$BASE_PORT)"
  rm -rf "$LOGDIR"
  exit 0
else
  echo "CLUSTER FAIL — full logs kept in $LOGDIR"
  exit 1
fi
