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

echo ============================================================
echo  All benchmarks complete.
echo ============================================================
pause
