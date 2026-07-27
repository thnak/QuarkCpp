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

echo ============================================================
echo  All benchmarks complete.
echo ============================================================
pause
