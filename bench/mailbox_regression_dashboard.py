#!/usr/bin/env python3
"""Mailbox subsystem regression dashboard — dimension 26 of the mailbox benchmark/test suite.

Runs the FULL new mailbox bench suite (dims 8-25) plus the correctness suite (dims 1-7), extracts
the headline number for each of the 25 named dimensions from the real printed output (never
fabricated), renders a single consolidated table, and diffs against a checked-in baseline file
(bench/mailbox_baseline_r8.json, captured from THIS session's run against the CURRENT shipped
mailbox) so a future run — including one evaluating whatever design-debate round 9 produces against
the mailbox — can be compared side by side against this recorded baseline in one command.

No external dependencies (stdlib only), no network calls. Always exits 0: this is a reporting tool,
not a pass/fail gate (bench/ci_bench_gate.sh is the existing hard-veto gate over [MISS] tokens; this
script is broader-scope and comparative, per the commissioning brief for dimension 26).

Usage:
    python3 bench/mailbox_regression_dashboard.py [--bench-dir DIR] [--build-dir DIR]
                                                   [--update-baseline] [--html OUT.html]

    --bench-dir DIR       Directory holding the *_bench executables (default: <repo>/build/bench).
    --build-dir DIR       ctest build directory for the correctness section (default: <repo>/build).
    --update-baseline      Overwrite bench/mailbox_baseline_r8.json with THIS run's numbers.
    --html OUT.html        Also render a self-contained HTML report to OUT.html.

Machine safety: every bench subprocess is launched under `taskset -c 0-3` when taskset is available
(mirrors bench/ci_bench_gate.sh) — never wider by default. mailbox_scaling_bench's NUMA/--wide
sections are NOT run by this default dashboard pass (they need a wider core range); run them
manually per their own file banners and fold the numbers in by hand if you want them in the report.

Noise caveat: this is a SINGLE run compared against a single baseline. On a virtualized/hypervisor-
jittery host, run-to-run swings of 20%+ on an otherwise-stable metric are expected noise, not a real
regression (observed directly: sched_bench swung 11.4->5.4 M/s across two back-to-back runs on this
box with no code change). Treat a lone REGRESSION flag as "worth a second look," not proof — re-run
2-3 times and look for a metric that stays down before trusting it, and never wire this script's
output into a hard gate (bench/ci_bench_gate.sh already covers that with its own separate contract).
"""
import argparse
import html
import json
import os
import re
import shutil
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BASELINE = os.path.join(REPO_ROOT, "bench", "mailbox_baseline_r8.json")


def pin_prefix():
    if shutil.which("taskset"):
        ncpu = os.cpu_count() or 1
        cap = min(4, ncpu)
        return ["taskset", "-c", f"0-{cap - 1}"]
    return []


def run_bench(bench_dir, name, args=None, timeout=120):
    exe = os.path.join(bench_dir, name)
    if not os.path.isfile(exe) or not os.access(exe, os.X_OK):
        return None, f"(missing: {exe})"
    cmd = pin_prefix() + [exe] + (args or [])
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "(timed out)"
    if proc.returncode != 0:
        return None, f"(exit {proc.returncode}: {proc.stdout[-500:]}{proc.stderr[-500:]})"
    return proc.stdout, None


def first_float(pattern, text, group=1):
    # re.DOTALL: several patterns intentionally bridge a label on one line to a value on a later
    # line via `.*?` (e.g. "enqueue-only:.*?p50 = ([\\d.]+) ns") — without DOTALL, `.` never
    # matches '\n' and those patterns silently fail to match at all (a real bug caught during this
    # script's own development: dim 12's occupancy-1 p50 was silently dropped from the table
    # because of exactly this). Safe to enable unconditionally: no pattern here relies on `.`
    # NOT crossing a newline.
    m = re.search(pattern, text, re.DOTALL)
    if not m:
        return None
    try:
        return float(m.group(group))
    except (ValueError, IndexError):
        return None


def extract_metrics(bench_dir):
    """Returns an ordered dict: dim_label -> {metric_name: (value, unit)}."""
    dims = {}

    # --- dim 8/9/15/25: mailbox_scaling_bench --------------------------------------------------
    out, err = run_bench(bench_dir, "mailbox_scaling_bench", timeout=180)
    if out:
        d89 = {}
        for m in re.finditer(r"P=(\d+)\s+aggregate=\s*([\d.]+) M msg/s\s+per-producer avg=\s*([\d.]+)",
                              out):
            p, agg, avg = m.group(1), float(m.group(2)), float(m.group(3))
            d89[f"P={p} aggregate"] = (agg, "M msg/s")
            d89[f"P={p} per-producer avg"] = (avg, "M msg/s")
        dims["8) Throughput (P=1 aggregate)"] = {"msg/s": d89.get("P=1 aggregate", (None, ""))}
        dims["9) Producer scaling"] = d89

        d15 = {}
        for m in re.finditer(
                r"P=(\d+)\s+contended\(shared tail_\)=\s*([\d.]+) M msg/s \(P/P1=([\d.]+)x\)\s+"
                r"uncontended\(independent\)=\s*([\d.]+) M msg/s \(P/P1=([\d.]+)x\)", out):
            p = m.group(1)
            d15[f"P={p} contended"] = (float(m.group(2)), "M msg/s")
            d15[f"P={p} uncontended"] = (float(m.group(4)), "M msg/s")
        dims["15) Contention isolation"] = d15

        d1617 = {}
        ss = first_float(r"same-socket.*?:\s*([\d.]+) M msg/s", out)
        cs = first_float(r"cross-socket.*?:\s*([\d.]+) M msg/s", out)
        if ss is not None:
            d1617["16) same-socket"] = (ss, "M msg/s")
        if cs is not None:
            d1617["17) cross-socket"] = (cs, "M msg/s")
        if not d1617:
            d1617["(skipped: single-socket box or /sys unavailable)"] = (None, "")
        dims["16/17) NUMA same vs cross socket"] = d1617

        d25 = {}
        spread = re.search(r"spread: p50 ([\d.]+)%\s+p99 ([\d.]+)%", out)
        if spread:
            d25["p50 spread"] = (float(spread.group(1)), "%")
            d25["p99 spread"] = (float(spread.group(2)), "%")
        dims["25) Fairness (per-producer latency spread)"] = d25
    else:
        dims["9) Producer scaling"] = {"error": (None, err)}
        dims["15) Contention isolation"] = {"error": (None, err)}
        dims["16/17) NUMA same vs cross socket"] = {"error": (None, err)}
        dims["25) Fairness (per-producer latency spread)"] = {"error": (None, err)}

    # --- dim 10: mailbox_consumer_scaling_bench ------------------------------------------------
    out, err = run_bench(bench_dir, "mailbox_consumer_scaling_bench", timeout=180)
    d10 = {}
    if out:
        for m in re.finditer(r"shards=workers=(\d+)\s+:\s*([\d.]+) M msg/s", out):
            d10[f"shards=workers={m.group(1)}"] = (float(m.group(2)), "M msg/s")
    else:
        d10["error"] = (None, err)
    dims["10) Consumer/shard scaling"] = d10

    # --- dim 11: mailbox_mixed_workload_bench --------------------------------------------------
    out, err = run_bench(bench_dir, "mailbox_mixed_workload_bench", timeout=60)
    d11 = {}
    if out:
        tmps = first_float(r"aggregate ([\d.]+) M msg/s over", out)
        askp50 = first_float(r"ask round-trip latency.*?p50\s+=\s*([\d.]+) ns", out, 1) or \
                 first_float(r"p50\s+=\s*([\d.]+) ns\n\s+p99", out)
        if tmps is not None:
            d11["tell aggregate (mixed payloads, concurrent w/ ask)"] = (tmps, "M msg/s")
        if askp50 is not None:
            d11["ask p50 (concurrent w/ tell traffic)"] = (askp50, "ns")
    else:
        d11["error"] = (None, err)
    dims["11) Mixed workload (tell+ask, varying payload)"] = d11

    # --- dim 12: mailbox_enqueue_latency_bench -------------------------------------------------
    out, err = run_bench(bench_dir, "mailbox_enqueue_latency_bench", timeout=60)
    d12 = {}
    if out:
        parts = out.split("B) contended")
        solo_p50 = first_float(r"enqueue-only:.*?p50\s+=\s*([\d.]+) ns", parts[0], 1) if parts else None
        cont_p50 = first_float(r"p50\s+=\s*([\d.]+) ns", parts[1]) if len(parts) > 1 else None
        if solo_p50 is not None:
            d12["occupancy-1 p50"] = (solo_p50, "ns")
        if cont_p50 is not None:
            d12["contended (P=4) pooled p50"] = (cont_p50, "ns")
    else:
        d12["error"] = (None, err)
    dims["12) Enqueue latency"] = d12

    # --- dim 13: mailbox_e2e_engine_bench ------------------------------------------------------
    out, err = run_bench(bench_dir, "mailbox_e2e_engine_bench", timeout=60)
    d13 = {}
    if out:
        parts = out.split("B)")
        a_p50 = first_float(r"p50\s+=\s*([\d.]+) ns", parts[0]) if parts else None
        b_p50 = first_float(r"p50\s+=\s*([\d.]+) ns", parts[1]) if len(parts) > 1 else None
        if a_p50 is not None:
            d13["1-worker e2e p50"] = (a_p50, "ns")
        if b_p50 is not None:
            d13["2-worker e2e p50"] = (b_p50, "ns")
    else:
        d13["error"] = (None, err)
    dims["13) End-to-end latency (real Engine)"] = d13

    # --- dim 14/22: mailbox_backlog_queue_depth_bench ------------------------------------------
    out, err = run_bench(bench_dir, "mailbox_backlog_queue_depth_bench", timeout=60)
    d14, d22 = {}, {}
    if out:
        for m in re.finditer(r"depth=\s*(\d+)\s+p50=\s*([\d.]+) ns\s+p99=\s*([\d.]+) ns\s+"
                              r"p999=\s*([\d.]+) ns\s+mean", out):
            d14[f"depth={m.group(1)} p999"] = (float(m.group(4)), "ns")
        for m in re.finditer(r"depth=\s*(\d+)\s+([\d.]+) M msg/s\s+p50=\s*([\d.]+) ns", out):
            d22[f"depth={m.group(1)} throughput"] = (float(m.group(2)), "M msg/s")
    else:
        d14["error"] = (None, err)
        d22["error"] = (None, err)
    dims["14) Tail latency vs backlog depth"] = d14
    dims["22) Queue depth (steady-state)"] = d22

    # --- dim 18: false_sharing_bench ------------------------------------------------------------
    out, err = run_bench(bench_dir, "false_sharing_bench", timeout=30)
    d18 = {}
    if out:
        speedup = first_float(r"speedup from separating the lines:\s*([\d.]+)x", out)
        if speedup is not None:
            d18["padded/false-shared speedup"] = (speedup, "x")
    else:
        d18["error"] = (None, err)
    dims["18) False sharing"] = d18

    # --- dim 19: mailbox_bench (micro sections) -------------------------------------------------
    out, err = run_bench(bench_dir, "mailbox_bench", timeout=30)
    d19, d8 = {}, {}
    if out:
        thr = first_float(r"([\d.]+) M msg/s/core", out)
        if thr is not None:
            d8["mailbox_bench peak (single-thread tight loop)"] = (thr, "M msg/s")
        enq = first_float(r"enqueue-only.*?\n\s*([\d.]+) ns/op", out)
        deq = first_float(r"dequeue-only.*?\n\s*([\d.]+) ns/op", out)
        if enq is not None:
            d19["enqueue-only"] = (enq, "ns/op")
        if deq is not None:
            d19["dequeue-only"] = (deq, "ns/op")
    else:
        d19["error"] = (None, err)
    dims.setdefault("8) Throughput (P=1 aggregate)", {}).update(d8)
    dims["19) Micro benchmark (enqueue/dequeue-only)"] = d19

    # --- dim 20: mailbox_cache_locality_bench ---------------------------------------------------
    out, err = run_bench(bench_dir, "mailbox_cache_locality_bench", timeout=30)
    d20 = {}
    if out:
        ratio = first_float(r"cold/hot ratio:\s*([\d.]+)x", out)
        if ratio is not None:
            d20["cold/hot ratio"] = (ratio, "x")
    else:
        d20["error"] = (None, err)
    dims["20) Cache locality"] = d20

    # --- dim 21: mailbox_memory_footprint_bench -------------------------------------------------
    out, err = run_bench(bench_dir, "mailbox_memory_footprint_bench", timeout=30)
    d21 = {}
    if out:
        desc_b = first_float(r"sizeof\(Descriptor\)\s+=\s*(\d+)", out)
        mb_b = first_float(r"sizeof\(Mailbox\)\s+=\s*(\d+)", out)
        part_b = first_float(r"~= (\d+) B/partition", out)
        if desc_b is not None:
            d21["sizeof(Descriptor)"] = (desc_b, "B")
        if mb_b is not None:
            d21["sizeof(Mailbox)"] = (mb_b, "B")
        if part_b is not None:
            d21["fixed per-partition overhead"] = (part_b, "B")
    else:
        d21["error"] = (None, err)
    dims["21) Memory footprint"] = d21

    # --- dim 23: sched_bench (existing; cited, not duplicated) ----------------------------------
    out, err = run_bench(bench_dir, "sched_bench", timeout=60)
    d23 = {}
    if out:
        thr = first_float(r"([\d.]+) M msg/s/core\s+\(023 sustained", out)
        if thr is not None:
            d23["full-lifecycle throughput"] = (thr, "M msg/s")
    else:
        d23["error"] = (None, err)
    dims["23) Scheduler (existing sched_bench.cpp)"] = d23

    # --- dim 24: ping_pong_bench ------------------------------------------------------------------
    out, err = run_bench(bench_dir, "ping_pong_bench", timeout=60)
    d24 = {}
    if out:
        parts = out.split("B)")
        a_p50 = first_float(r"p50\s+=\s*([\d.]+) ns", parts[0]) if parts else None
        b_p50 = first_float(r"p50\s+=\s*([\d.]+) ns", parts[1]) if len(parts) > 1 else None
        if a_p50 is not None:
            d24["same-shard p50"] = (a_p50, "ns")
        if b_p50 is not None:
            d24["cross-shard p50"] = (b_p50, "ns")
    else:
        d24["error"] = (None, err)
    dims["24) Ping-pong round trip"] = d24

    return dims


def run_correctness(build_dir):
    """Runs the dimension 1-7 correctness tests via ctest and returns a summary dict."""
    tests = ["mailbox_dims_smoke_test", "mailbox_engine_fifo_exactly_once_test",
             "mailbox_pool_aba_stress_test", "mailbox_mpsc_test", "mailbox_cancel_test",
             "mailbox_noalloc_test", "message_pool_partition_concurrency_test"]
    results = {}
    if not os.path.isdir(build_dir):
        return {"error": f"build dir not found: {build_dir}"}
    cmd = pin_prefix() + ["ctest", "--test-dir", build_dir, "--output-on-failure"]
    regex = "|".join(tests)
    try:
        proc = subprocess.run(cmd + ["-R", regex], capture_output=True, text=True, timeout=120)
        results["ctest_output_tail"] = "\n".join(proc.stdout.strip().splitlines()[-20:])
        results["exit_code"] = proc.returncode
    except (subprocess.TimeoutExpired, FileNotFoundError) as e:
        results["error"] = str(e)
    return results


def flatten_baseline(dims):
    flat = {}
    for dim, metrics in dims.items():
        for k, (v, unit) in metrics.items():
            if v is not None:
                flat[f"{dim} :: {k}"] = {"value": v, "unit": unit}
    return flat


def print_table(dims):
    print("=" * 100)
    print("QUARK MAILBOX SUBSYSTEM — REGRESSION DASHBOARD (dimension 26)")
    print("Baseline: CURRENT shipped mailbox (include/quark/core/mailbox.hpp), pre round-9 redesign")
    print("=" * 100)
    for dim, metrics in dims.items():
        print(f"\n[{dim}]")
        if not metrics:
            print("  (no data captured)")
            continue
        for k, (v, unit) in metrics.items():
            if v is None:
                print(f"  {k}: {unit}")
            else:
                print(f"  {k:<55} {v:>12.3f} {unit}")


def diff_against_baseline(current_flat, baseline_path):
    if not os.path.isfile(baseline_path):
        print(f"\n[baseline] no baseline file at {baseline_path} yet — nothing to diff against.")
        return
    with open(baseline_path) as f:
        baseline = json.load(f)
    baseline_flat = baseline.get("metrics", {})
    print(f"\n[baseline diff] vs {baseline_path} (captured {baseline.get('captured_at', '?')})")
    print("NOTE: single-run diff on a virtualized host is noisy — a lone REGRESSION flag below is "
          "not proof of a real regression; re-run before trusting it (see module docstring).")
    print("-" * 100)
    for key, cur in current_flat.items():
        if key not in baseline_flat:
            print(f"  NEW      {key}: {cur['value']:.3f} {cur['unit']}")
            continue
        old = baseline_flat[key]["value"]
        if old == 0:
            continue
        delta_pct = 100.0 * (cur["value"] - old) / abs(old)
        flag = ""
        # Regression heuristic: throughput/x/M-msg-s metrics regress when they DROP; latency/ns
        # metrics regress when they RISE. "%"/spread metrics: rising is worse (less fair).
        lower_is_better = cur["unit"] in ("ns", "%")
        got_worse = (delta_pct < 0) if not lower_is_better else (delta_pct > 0)
        if abs(delta_pct) >= 20 and got_worse:
            flag = "  <-- REGRESSION (>=20% worse)"
        elif abs(delta_pct) >= 20:
            flag = "  <-- improved (>=20% better)"
        print(f"  {key:<70} {old:>10.3f} -> {cur['value']:>10.3f} {cur['unit']:<8} "
              f"({delta_pct:+.1f}%){flag}")


def render_html(dims, correctness, out_path):
    rows = []
    for dim, metrics in dims.items():
        for k, (v, unit) in metrics.items():
            val = f"{v:.3f} {unit}" if v is not None else html.escape(str(unit))
            rows.append(f"<tr><td>{html.escape(dim)}</td><td>{html.escape(k)}</td>"
                        f"<td>{html.escape(val)}</td></tr>")
    body = f"""<title>Quark Mailbox Regression Dashboard</title>
<style>
body {{ font-family: -apple-system, sans-serif; margin: 2rem; max-width: 1000px; }}
table {{ border-collapse: collapse; width: 100%; }}
td, th {{ border: 1px solid #8883; padding: 6px 10px; text-align: left; }}
th {{ background: #8882; }}
code {{ background: #8882; padding: 2px 4px; border-radius: 3px; }}
</style>
<h1>Quark Mailbox Subsystem Regression Dashboard</h1>
<p>Baseline: current shipped mailbox (<code>include/quark/core/mailbox.hpp</code>), captured
{time.strftime("%Y-%m-%d %H:%M:%S")}.</p>
<h2>Correctness (dims 1-7)</h2>
<pre>{html.escape(str(correctness))}</pre>
<h2>Performance (dims 8-25)</h2>
<table><tr><th>Dimension</th><th>Metric</th><th>Value</th></tr>
{"".join(rows)}
</table>
"""
    with open(out_path, "w") as f:
        f.write(body)
    print(f"\n[html] wrote {out_path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench-dir", default=os.path.join(REPO_ROOT, "build", "bench"))
    ap.add_argument("--build-dir", default=os.path.join(REPO_ROOT, "build"))
    ap.add_argument("--baseline", default=DEFAULT_BASELINE)
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--html", default=None)
    args = ap.parse_args()

    print(f"bench dir: {args.bench_dir}")
    print(f"build dir (ctest): {args.build_dir}")
    if not pin_prefix():
        print("[warn] taskset unavailable — running unpinned")

    print("\n--- Correctness suite (dims 1-7) ---")
    correctness = run_correctness(args.build_dir)
    print(correctness.get("ctest_output_tail", correctness.get("error", "(no output)")))

    print("\n--- Performance suite (dims 8-25) ---")
    dims = extract_metrics(args.bench_dir)
    print_table(dims)

    flat = flatten_baseline(dims)
    diff_against_baseline(flat, args.baseline)

    if args.update_baseline or not os.path.isfile(args.baseline):
        payload = {"captured_at": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime()),
                  "note": "Baseline for the CURRENT shipped Vyukov mailbox (include/quark/core/"
                          "mailbox.hpp), pre round-9 redesign. Re-run with --update-baseline after "
                          "a future mailbox redesign lands to compare, or diff this file directly.",
                  "metrics": flat}
        os.makedirs(os.path.dirname(args.baseline), exist_ok=True)
        with open(args.baseline, "w") as f:
            json.dump(payload, f, indent=2, sort_keys=True)
        print(f"\n[baseline] wrote {args.baseline}")

    if args.html:
        render_html(dims, correctness, args.html)

    print("\nmailbox_regression_dashboard: done (informational — always exits 0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
