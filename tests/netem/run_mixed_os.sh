#!/usr/bin/env bash
# Issue #17 — real cross-OS mixed cluster: the SEED runs on a remote host (any OS), the JOINER runs here,
# reaching the seed's loopback listener through an ssh -L tunnel. Both processes advertise their genuine
# pal::platform_abi_tag, so a Windows<->Linux pair MUST take the canonical tagged fallback; the same-OS
# positive control (run_mixed_os.sh --control) must take the tagless fast path.
#
# Usage: tests/netem/run_mixed_os.sh <local-build-dir> <ssh-host-alias> <remote-build-dir> [port=29301] [N=50]
#        tests/netem/run_mixed_os.sh --control <local-build-dir> [port=29301] [N=50]   # both nodes local
# Needs: tests/mixed_os_node built on BOTH hosts (cmake --build <dir> --target mixed_os_node), ssh access.
set -u
if [ "${1:-}" = "--control" ]; then
  BUILD=${2:?local build dir}; PORT=${3:-29301}; N=${4:-50}
  BIN="$BUILD/tests/mixed_os_node"; [ -x "$BIN" ] || BIN="$BIN.exe"
  "$BIN" a "$PORT" "$N" tagless 60 > /tmp/mixed_os_a.out 2>&1 &
  APID=$!; sleep 1
  "$BIN" b "$PORT" "$N" tagless 60; RB=$?
  wait $APID; RA=$?; cat /tmp/mixed_os_a.out
  [ $RA -eq 0 ] && [ $RB -eq 0 ] && echo "mixed_os control: PASS" || { echo "mixed_os control: FAIL"; exit 1; }
  exit 0
fi
BUILD=${1:?local build dir}; HOST=${2:?ssh host alias}; RBUILD=${3:?remote build dir}
PORT=${4:-29301}; N=${5:-50}
BIN="$BUILD/tests/mixed_os_node"; [ -x "$BIN" ] || BIN="$BIN.exe"
ssh "$HOST" "rm -f /tmp/mixed_os_a.out; (setsid nohup $RBUILD/tests/mixed_os_node a $PORT $N auto 90 > /tmp/mixed_os_a.out 2>&1 &); sleep 1" || exit 2
ssh -N -o ExitOnForwardFailure=yes -L "$PORT:127.0.0.1:$PORT" "$HOST" & TPID=$!
sleep 3
"$BIN" b "$PORT" "$N" auto 60; RB=$?
sleep 3
kill $TPID 2>/dev/null
RA_OUT=$(ssh "$HOST" "cat /tmp/mixed_os_a.out")
echo "$RA_OUT"
echo "$RA_OUT" | grep -q -- '-> PASS' && [ $RB -eq 0 ] && echo "mixed_os cross-host: PASS" || { echo "mixed_os cross-host: FAIL"; exit 1; }
