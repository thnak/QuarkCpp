#!/usr/bin/env bash
# Objdump proof for 024-Streaming §0-cross-core-RMW-on-drain (ADR-005 S1). Disassembles the
# `quark_stream_drain_probe` symbol in the given binary and asserts the drain inner loop contains NO
# cross-core RMW / fence mnemonic: no `lock`-prefixed op, no `cmpxchg`, no `xadd`, no memory-operand
# `xchg`, no `mfence`. Gated to NON-sanitizer builds by CMake (sanitizer instrumentation adds locks).
set -euo pipefail

BIN="${1:?usage: stream_drain_rmw_check.sh <binary>}"
SYM="quark_stream_drain_probe"

if ! command -v objdump >/dev/null 2>&1; then
  echo "stream_drain_zero_rmw: SKIP (objdump not available)"
  exit 0
fi

# Slice the disassembly of the probe function (from its label to the next blank line). NOTE: awk must
# NOT `exit` early here — closing the pipe while objdump is still writing gives objdump a SIGPIPE, and
# under `set -o pipefail` that fails the whole pipeline (flaky: hit only for larger binaries where
# objdump is still mid-write). Instead we keep reading to EOF and just stop appending after the blank
# line (`stop`), so the pipeline always drains cleanly.
DIS="$(objdump -d "$BIN" | awk -v s="<${SYM}>:" 'index($0,s){f=1} f && !stop {print; if (NF==0) stop=1}')"

if [ -z "$DIS" ]; then
  echo "stream_drain_zero_rmw: FAIL (symbol ${SYM} not found in ${BIN})"
  exit 1
fi

# `lock`-prefixed RMW, cmpxchg, xadd, mfence — any cross-core RMW/fence. `xchg` with a memory operand
# (implicitly locked) is caught separately; `xchg %reg,%reg` (a nop hint) is deliberately NOT matched.
BAD="$(echo "${DIS}" | grep -Ei '(\block\b|cmpxchg|xadd|mfence|lfence|sfence)' || true)"
BADXCHG="$(echo "${DIS}" | grep -Ei 'xchg[[:space:]].*\(%' || true)"

if [ -n "${BAD}" ] || [ -n "${BADXCHG}" ]; then
  echo "stream_drain_zero_rmw: FAIL (cross-core RMW / fence found in ${SYM}):"
  [ -n "${BAD}" ] && echo "${BAD}"
  [ -n "${BADXCHG}" ] && echo "${BADXCHG}"
  exit 1
fi

echo "stream_drain_zero_rmw: OK (0 lock/cmpxchg/xadd/xchg-mem/mfence in ${SYM})"
exit 0
