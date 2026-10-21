#!/bin/bash
# Multi-process SWIM (021 §3) chaos scenarios over real TcpTransport sockets. Linux only; needs sudo.
#
# Topology: N node namespaces (qn<i>, address 10.77.0.<i>/24), each joined by a veth pair to one bridge
# living in its own hub namespace. Nothing is created in the host's namespace, so the host's
# interfaces, routes and firewall are never touched; everything is removed on exit.
# Faults are injected inside the node namespaces only:
#   partition — iptables DROP of the peer's address, both directions (a silent drop, like a real cut)
#   impair    — tc netem on the node's own interface
#
# Each node runs tests/netem/swim_chaos_node.cpp and prints its view of every peer every 250 ms; this
# script prints those views at each phase boundary (before fault / end of fault / final).
#
# Usage: tests/netem/run_swim_chaos.sh <build-dir> [scenario...]
#   scenarios: form isolate asym split flaky   (default: all)
set -u
BUILD=${1:?usage: run_swim_chaos.sh <build-dir> [scenario...]}
shift
SCENARIOS=("$@")
[ ${#SCENARIOS[@]} -gt 0 ] || SCENARIOS=(form isolate asym split flaky)
BIN=$(realpath "$BUILD")/tests/swim_chaos_node
[ -x "$BIN" ] || { echo "missing $BIN (build target swim_chaos_node)"; exit 1; }
N=5
PORT=7400
HUB=qhub
OUT=${OUT:-swim-chaos-results}
RUN_AS=$(id -un)
mkdir -p "$OUT"

nsx() { local ns=$1; shift; sudo ip netns exec "$ns" "$@"; }

teardown() {
  for i in $(seq 1 $N); do sudo ip netns del qn$i 2>/dev/null; done
  sudo ip netns del $HUB 2>/dev/null
}
trap teardown EXIT

setup() {
  teardown
  sudo ip netns add $HUB
  nsx $HUB ip link add br0 type bridge
  nsx $HUB ip link set br0 up
  nsx $HUB ip link set lo up
  for i in $(seq 1 $N); do
    sudo ip netns add qn$i
    sudo ip link add vn$i netns qn$i type veth peer name vh$i netns $HUB
    nsx qn$i ip link set vn$i name eth0
    nsx qn$i ip addr add 10.77.0.$i/24 dev eth0
    nsx qn$i ip link set eth0 up
    nsx qn$i ip link set lo up
    nsx $HUB ip link set vh$i master br0
    nsx $HUB ip link set vh$i up
  done
}

# cut <a> <b>: drop all traffic between nodes a and b (both directions, enforced on both sides).
cut() {
  nsx qn$1 iptables -A INPUT -s 10.77.0.$2 -j DROP; nsx qn$1 iptables -A OUTPUT -d 10.77.0.$2 -j DROP
  nsx qn$2 iptables -A INPUT -s 10.77.0.$1 -j DROP; nsx qn$2 iptables -A OUTPUT -d 10.77.0.$1 -j DROP
}
heal() { for i in $(seq 1 $N); do nsx qn$i iptables -F; done; }

# start <duration_ms> <rejoin>: node 1 is the seed, the rest join it.
start() {
  PIDS=()
  for i in $(seq 1 $N); do
    local seed=1
    [ "$i" = 1 ] && seed=0
    nsx qn$i sudo -u "$RUN_AS" "$BIN" $i $N $seed $PORT "$1" "$2" > "$OUT/$SC.node$i.log" 2>&1 &
    PIDS+=($!)
    [ "$i" = 1 ] && sleep 0.3
  done
}

# snap <label> <t_ms>: each node's last sample at or before t_ms.
snap() {
  printf '  %-14s' "$1"
  for i in $(seq 1 $N); do
    local line
    line=$(awk -v t="$2" '$1=="T" && $2<=t {l=$0} END{print l}' "$OUT/$SC.node$i.log")
    printf ' n%s:%s' "$i" "$(sed -n 's/.*st=\([^ ]*\).*/\1/p' <<<"$line")"
  done
  echo
}

for SC in "${SCENARIOS[@]}"; do
  setup
  echo "== $SC"
  case $SC in
    form)  # 5 nodes join seed 1; no faults. Expect every view all-alive within a few seconds.
      start 15000 0; wait "${PIDS[@]}"
      snap "t=3s" 3000; snap "final" 99999999 ;;
    isolate)  # node 5 cut off from everyone 8s..18s, then healed (rejoin=1).
      start 40000 1; sleep 8
      for j in 1 2 3 4; do cut 5 $j; done; sleep 10; heal; wait "${PIDS[@]}"
      snap "before(7s)" 7000; snap "cut(17s)" 17000; snap "final(40s)" 99999999 ;;
    asym)  # ONLY the 1<->2 link cut from 8s on; 3,4,5 can still reach both. Correct SWIM keeps 1 and 2
           # alive in each other's views via indirect ping-req through 3/4/5.
      start 30000 0; sleep 8
      cut 1 2; wait "${PIDS[@]}"
      snap "before(7s)" 7000; snap "cut(15s)" 15000; snap "final(30s)" 99999999 ;;
    split)  # {1,2} | {3,4,5} 8s..20s, then healed (rejoin=1).
      start 45000 1; sleep 8
      for a in 1 2; do for b in 3 4 5; do cut $a $b; done; done; sleep 12; heal; wait "${PIDS[@]}"
      snap "before(7s)" 7000; snap "split(19s)" 19000; snap "final(45s)" 99999999 ;;
    flaky)  # node 3's link: 20% loss + 50ms±20ms for the whole run. Expect no node declared dead.
      nsx qn3 tc qdisc add dev eth0 root netem delay 50ms 20ms loss 20%
      start 30000 0; wait "${PIDS[@]}"
      snap "t=10s" 10000; snap "final(30s)" 99999999 ;;
    *) echo "unknown scenario $SC"; continue ;;
  esac
done
