# Quark hot-path microbenchmarks (percentiles vs the 023 budgets). Auto-discovered like tests:
# drop a `*_bench.cpp` here and reconfigure. Run pinned: `taskset -c 0 build/bench/<name>` for
# single-thread latency, `-c 0-3` for multi-thread throughput. NEVER saturate all cores.

find_package(Threads REQUIRED)

file(GLOB QUARK_BENCH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_bench.cpp")
set(_quark_bench_targets "")
foreach(src ${QUARK_BENCH_SOURCES})
  get_filename_component(name "${src}" NAME_WE)
  add_executable(${name} "${src}")
  target_link_libraries(${name} PRIVATE quark::quark quark_warnings Threads::Threads)
  target_compile_options(${name} PRIVATE -O3)
  list(APPEND _quark_bench_targets ${name})
endforeach()

# 007/ADR-009 zero-cost proof: the CONTROL twin of supervision_bench with the handler-boundary guard
# compiled OUT (-DQUARK_SUPERVISION_NO_GUARD). Compare its p99 against the guarded build (ADR-009 F1).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/supervision_bench.cpp")
  add_executable(supervision_bench_noguard supervision_bench.cpp)
  target_link_libraries(supervision_bench_noguard PRIVATE quark::quark quark_warnings Threads::Threads)
  target_compile_options(supervision_bench_noguard PRIVATE -O3)
  target_compile_definitions(supervision_bench_noguard PRIVATE QUARK_SUPERVISION_NO_GUARD)
endif()

# ---- 023 performance regression gate ----------------------------------------------------------
# `cmake --build <dir> --target bench-gate` builds every bench and runs ci_bench_gate.sh, which
# FAILS (non-zero) on a Hard-budget violation ([MISS]) and WARNS (green) on a Goal regression
# ([hard]/[floor]) — the fail-on-hard/warn-on-goal contract of 023 §"Regression gating". This is
# the entry point CI calls (.github/workflows/ci.yml). Perf is only meaningful on a native release
# build (023 §"Why not the deterministic simulator": real hardware, wall clock), so the gate is not
# created under a sanitizer build — ASan/TSan instrumentation makes ns-scale numbers meaningless.
if(NOT QUARK_SANITIZE AND _quark_bench_targets)
  add_custom_target(bench-gate
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/ci_bench_gate.sh" "$<TARGET_FILE_DIR:mailbox_bench>"
    DEPENDS ${_quark_bench_targets}
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    USES_TERMINAL
    COMMENT "023 bench gate: FAIL on Hard-budget [MISS], WARN on Goal regression")

  # Opt-in ctest entry (default OFF): perf gating belongs on a pinned, quiesced machine (023
  # §"Regression gating"), not every local `ctest`, and a full bench sweep is slow — so it is NOT
  # in the default test set. Enable with -DQUARK_BENCH_CI_GATE=ON (CI does), then `ctest -L perf`.
  option(QUARK_BENCH_CI_GATE "Register the 023 bench gate as a ctest test (label: perf)" OFF)
  if(QUARK_BENCH_CI_GATE)
    add_test(NAME bench_regression_gate
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/ci_bench_gate.sh" "$<TARGET_FILE_DIR:mailbox_bench>")
    # RUN_SERIAL: the benches need the pinned cores to themselves — no other test may run alongside.
    set_tests_properties(bench_regression_gate PROPERTIES LABELS "perf" RUN_SERIAL TRUE TIMEOUT 600)
  endif()
endif()
