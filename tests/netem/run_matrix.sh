#!/bin/bash
# Network-impairment matrix for the real-socket transport tests (010 Distribution / 020 Security /
# 028 Voice over TcpTransport). Linux only; needs sudo for `ip netns` + `tc`.
#
# Every test runs in its OWN fresh network namespace with `tc netem` on that namespace's private
# loopback. The tests talk over 127.0.0.1, so they run unchanged. The host's real interfaces and its
# own `lo` are never touched. A fresh namespace per test matters: a test that exits with packets
# still queued in a rate-limited qdisc would otherwise delay the next test's packets (seen once as a
# false dial-dedup "failure" on a 1 Mbit link).
#
# Usage: tests/netem/run_matrix.sh <build-dir> [reps] [out-dir]
set -u
BUILD=${1:?usage: run_matrix.sh <build-dir> [reps] [out-dir]}
REPS=${2:-3}
OUT=${3:-netem-results}
NS=quark-netem-$$
RUN_AS=$(id -un)
TESTS_RE='tcp_|security_rotation_sweep|security_secure_transport_handshake_timeout|security_secure_transport_peer_disconnect|voice_channel_loopback'

PROFILES=(
  "00_baseline|"
  "01_delay5ms_jitter2|delay 5ms 2ms"
  "02_delay50ms_jitter10|delay 50ms 10ms"
  "03_loss1|loss 1%"
  "04_loss5|loss 5%"
  "05_reorder25|delay 10ms reorder 25% 50%"
  "06_dup_corrupt|duplicate 1% corrupt 0.1%"
  "07_rate1mbit|rate 1mbit delay 2ms"
  "08_wan|delay 80ms 20ms distribution normal loss 0.5% reorder 2%"
)

cleanup() { sudo ip netns del "$NS" 2>/dev/null; }
trap cleanup EXIT

mapfile -t TESTS < <(ctest --test-dir "$BUILD" -N -R "$TESTS_RE" | sed -n 's/^ *Test *#[0-9]*: //p')
[ ${#TESTS[@]} -gt 0 ] || { echo "no matching tests in $BUILD"; exit 1; }
mkdir -p "$OUT"
: > "$OUT/summary.txt"
fail=0

for entry in "${PROFILES[@]}"; do
  name=${entry%%|*}
  netem=${entry#*|}
  for rep in $(seq 1 "$REPS"); do
    pass=0
    failed=()
    for t in "${TESTS[@]}"; do
      cleanup
      sudo ip netns add "$NS" && sudo ip netns exec "$NS" ip link set lo up
      [ -n "$netem" ] && sudo ip netns exec "$NS" tc qdisc add dev lo root netem $netem limit 100000
      log="$OUT/$name.rep$rep.$t.log"
      if sudo ip netns exec "$NS" sudo -u "$RUN_AS" \
           ctest --test-dir "$BUILD" -R "^${t}\$" --output-on-failure --timeout 300 > "$log" 2>&1; then
        pass=$((pass + 1))
      else
        failed+=("$t")
        fail=1
      fi
    done
    line="$name rep$rep: $pass/${#TESTS[@]} passed${failed:+  FAILED: ${failed[*]}}"
    echo "$line" | tee -a "$OUT/summary.txt"
  done
done
exit $fail
