@echo off
setlocal enabledelayedexpansion

echo ============================================================
echo  Quark v0.1.0 vs CAF v1.1.0 — Full Benchmark Suite
echo  Machine: AMD Ryzen 5 4600H (6C/12T), Windows
echo ============================================================
echo.

REM ---- Single-threaded (1:1 fair comparison) ----
echo ========== 1. SINGLE-THREADED (1 worker each) ==========
echo.
echo --- Quark 1 thread ---
quark_bench.exe 1
echo.
echo --- CAF 1 thread ---
caf_bench.exe 1
echo.

REM ---- Max threads ----
echo ========== 2. MAX THREADS (12 workers each) ==========
echo.
echo --- Quark 12 workers ---
quark_bench.exe 0
echo.
echo --- CAF 12 threads ---
caf_bench.exe 0
echo.

REM ---- MPSC scaling ----
echo ========== 3. MPSC SCALING (multi-producer -> 1 actor) ==========
echo.
for %%p in (1 2 4 8 12) do (
    echo --- Quark %%p producer(s) ---
    quark_mpsc_bench.exe %%p
    echo.
)
for %%p in (1 2 4 8 12) do (
    echo --- CAF %%p producer(s) ---
    caf_mpsc_bench.exe %%p
    echo.
)

REM ---- Sharded MPSC scaling (N producers -> N actors, contention-free ceiling) ----
REM Capped at 4 (CLAUDE.md machine-safety rule: this repo caps multi-thread stress at 4
REM threads on this box; unlike the 8/12 rows above, this loop was written after that rule
REM was already in force, so it isn't extended past 4).
echo ========== 4. SHARDED MPSC SCALING (N producers -^> N actors) ==========
echo.
for %%p in (1 2 4) do (
    echo --- Quark sharded, %%p producer(s)/actor(s) ---
    quark_mpsc_sharded_bench.exe %%p
    echo.
)
for %%p in (1 2 4) do (
    echo --- CAF sharded, %%p producer(s)/actor(s) ---
    caf_mpsc_sharded_bench.exe %%p
    echo.
)

REM ---- Sustained stress test (whole-engine survivability, both engines) ----
REM Kept at 1-2 pairs here (2-4 total OS threads, at/under CLAUDE.md's standing 4-thread cap) so
REM the unattended sweep stays safe. Higher pair counts (4/6/12+) are a genuine stress/soak test
REM that can pin all cores for the full duration and should be run manually, ramped up gradually —
REM see README.md's "Sustained stress test" section for the 1-12 pair results (both engines) and
REM why (an earlier, unsafe version of the Quark bench made the dev box unresponsive; both
REM *_stress_bench.exe's current safety design — no ask()/request() during the flood, a hard send
REM cap, a process watchdog — is what makes even the manual higher-pair runs safe now, but this
REM automated sweep still stays conservative).
echo ========== 5. SUSTAINED STRESS TEST (paired producer/actor lanes) ==========
echo.
for %%p in (1 2) do (
    echo --- Quark stress, %%p pair(s), 3s sustained ---
    quark_stress_bench.exe %%p 3 1
    echo.
)
for %%p in (1 2) do (
    echo --- CAF stress, %%p pair(s), 3s sustained ---
    caf_stress_bench.exe %%p 3 1
    echo.
)

echo ============================================================
echo  All benchmarks complete.
echo ============================================================
pause
