# Quark tests. Sources are auto-discovered so parallel module work needs no shared edits here:
# drop a `*_test.cpp` in this dir and reconfigure. Each becomes a CTest target linked against the
# core + warnings + threads. Run capped: `taskset -c 0-3 ctest --test-dir build` — never saturate.
#
# NOTE: GLOB is resolved at configure time, so re-run cmake (or `cmake --build`, which reconfigures)
# after adding a file. New modules: add one `*_test.cpp` per load-bearing invariant (CONVENTIONS.md).

find_package(Threads REQUIRED)

file(GLOB QUARK_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
list(APPEND QUARK_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.cpp")
list(REMOVE_DUPLICATES QUARK_TEST_SOURCES)

# ThreadSanitizer's runtime (libclang_rt.tsan_cxx) DEFINES its own strong global operator new/delete.
# A test that REPLACES global operator new (the 0-allocation probes do, to count every allocation)
# therefore collides at link under TSan ("multiple definition of operator new") and the whole TSan
# build fails. TSan is also not where zero-allocation is measured — the sanitizer allocates on its own
# — so those tests carry no TSan value. We exclude them from a TSan build ONLY, and detect them by
# CONTENT (a global `operator new(` replacement), NOT by filename: a name-based skip is exactly why a
# newly-added probe (governance_zero_cost_test) silently broke the clean TSan link before. The 0-alloc
# invariant stays fully verified by the gcc / clang / ASan builds (ASan intercepts malloc instead of
# defining operator new, so the replacement links and the counts are exact there).
set(_quark_tsan OFF)
if(QUARK_SANITIZE MATCHES "thread")
  set(_quark_tsan ON)
endif()

foreach(src ${QUARK_TEST_SOURCES})
  get_filename_component(name "${src}" NAME_WE)
  # config_hotcell_read_asm_test disassembles ITS OWN BINARY via objdump against /proc/self/exe (ADR-008
  # F1) — a GNU-toolchain/Linux-only static-analysis gate the test's own banner scopes to "g++ and
  # clang++"; it neither compiles (no <unistd.h>/objdump on MSVC) nor would mean anything against
  # MSVC-generated PE/COFF codegen. Skip it on Windows; the invariant stays fully gated on Linux/arm64.
  if(WIN32 AND name STREQUAL "config_hotcell_read_asm_test")
    message(STATUS "Quark: skipping ${name} on Windows (objdump/proc-self-exe gate is Linux/GNU-only)")
    continue()
  endif()
  if(_quark_tsan)
    file(READ "${src}" _src_text)
    # Match a global replacement definition: `void* operator new(` / `void operator delete(` at
    # namespace scope. (A mere mention in a comment lacks the `void`/`void*` return + `(`.)
    if(_src_text MATCHES "operator new\\(std::size_t" OR _src_text MATCHES "operator new\\[\\]\\(std::size_t")
      message(STATUS "Quark: skipping ${name} under TSan (replaces global operator new — collides with tsan_cxx)")
      continue()
    endif()
  endif()
  add_executable(${name} "${src}")
  target_link_libraries(${name} PRIVATE quark::quark quark_warnings Threads::Threads)
  add_test(NAME ${name} COMMAND ${name})
endforeach()

# --- Special targets (positive controls / -D variants) that can't be auto-discovered ----------
# The no-lost-wakeup CONTROL: -DQUARK_SCHED_BROKEN_WAKEUP drops the park Dekker barrier+rescan, so
# the harness MUST strand — proving it detects a lost wakeup. The control binary returns 0 iff it
# strands (002). Guarded so it only builds when its source exists.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sched_no_lost_wakeup_test.cpp")
  add_executable(sched_no_lost_wakeup_control sched_no_lost_wakeup_test.cpp)
  target_link_libraries(sched_no_lost_wakeup_control PRIVATE quark::quark quark_warnings Threads::Threads)
  target_compile_definitions(sched_no_lost_wakeup_control PRIVATE QUARK_SCHED_BROKEN_WAKEUP)
  add_test(NAME sched_no_lost_wakeup_control COMMAND sched_no_lost_wakeup_control)
endif()

# The 016 fingerprint-mismatch OOB CONTROL (ADR-016 control 4, ASan half): forces the unchecked
# tagless reader over a tight, exact-sized heap buffer with a longer/mismatched schema, so it
# over-reads. Under ASan this aborts with a heap-buffer-overflow; WILL_FAIL means a non-zero exit
# (the trap) == the control firing. Best run in build-asan. (The auto-discovered
# serialize_fingerprint_mismatch_control_test is the in-bounds wrong-value half — it exits 0.)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/serialize_fingerprint_mismatch_control_test.cpp")
  add_executable(serialize_fingerprint_mismatch_oob serialize_fingerprint_mismatch_control_test.cpp)
  target_link_libraries(serialize_fingerprint_mismatch_oob PRIVATE quark::quark quark_warnings Threads::Threads)
  target_compile_definitions(serialize_fingerprint_mismatch_oob PRIVATE QUARK_CONTROL_OOB)
  add_test(NAME serialize_fingerprint_mismatch_oob COMMAND serialize_fingerprint_mismatch_oob)
  # symbolize=0: this is the ONLY test that deliberately triggers an ASan report, so it is the only one
  # that reaches ASan's stack symbolization. When llvm-symbolizer is absent (this box has addr2line
  # only), ASan's default symbolizer spawn HANGS on the subprocess pipe (blocked in pipe_read) instead
  # of failing fast — the trap fires but the process never exits, so the WILL_FAIL test TIMES OUT. The
  # control only needs the non-zero exit (the trap), not a symbolized trace, so disable symbolization to
  # make it robust to the missing symbolizer. Keep the ASan default abort_on_error=0 so the trap is a
  # clean exit(exitcode=1) — a SIGABRT would read to CTest as a crash, which WILL_FAIL does NOT invert.
  set_tests_properties(serialize_fingerprint_mismatch_oob PROPERTIES
                       WILL_FAIL TRUE
                       ENVIRONMENT "ASAN_OPTIONS=symbolize=0:abort_on_error=0:exitcode=1")
endif()

# The 007/ADR-009 restart-episode-marker CONTROL: -DQUARK_SUPERVISION_NO_EPISODE_MARKER drops the
# marker so two concurrently-faulting siblings DOUBLE-restart (restarts_total == 2 for one episode).
# The control binary self-checks for the doubled count (it exits 0 iff it double-restarts), so the
# 1-vs-2 delta against supervision_episode_test proves the marker's effect (ADR-009 D3-S3).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/supervision_episode_test.cpp")
  add_executable(supervision_episode_control supervision_episode_test.cpp)
  target_link_libraries(supervision_episode_control PRIVATE quark::quark quark_warnings Threads::Threads)
  target_compile_definitions(supervision_episode_control PRIVATE QUARK_SUPERVISION_NO_EPISODE_MARKER)
  add_test(NAME supervision_episode_control COMMAND supervision_episode_control)
endif()

# The 025 Stateless-pool SHARED-STATE CONTROL (ADR-011 Gate C teeth): -DQUARK_STATELESS_SHARED_STATE_
# CONTROL makes every pool activation RMW ONE shared plain accumulator, violating the per-activation
# discipline. With the overlap gate forcing genuine concurrency, the torn counter CATCHES it (torn>0);
# the control binary exits 0 iff it detected tearing, so it is a normal passing test here.
#
# NOT added under TSan: a CONTINUOUSLY-contended data race (both activations hammering one location the
# whole run) is a known TSan pathology — race analysis on a hot shared location runs for minutes. The
# real pool (the main test) IS a TSan test and stays TSan-clean; the control's TSan *report* teeth is
# demonstrated out-of-band (the shared RMW trips ThreadSanitizer at the handle() site, exit non-zero) —
# it is simply not wired as a permanent, slow TSan ctest entry (mirrors the operator-new TSan skips).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stateless_pool_exactly_once_test.cpp" AND NOT _quark_tsan)
  add_executable(stateless_pool_shared_state_control stateless_pool_exactly_once_test.cpp)
  target_link_libraries(stateless_pool_shared_state_control PRIVATE quark::quark quark_warnings Threads::Threads)
  target_compile_definitions(stateless_pool_shared_state_control PRIVATE QUARK_STATELESS_SHARED_STATE_CONTROL)
  add_test(NAME stateless_pool_shared_state_control COMMAND stateless_pool_shared_state_control)
endif()

# --- 024-Streaming ADR-014 exactly-once-across-suspend gate CONTROLS (must FIRE non-vacuously) -------
# CONTROL-4 (single cursor): -DQUARK_STREAM_SINGLE_CURSOR collapses disp/tail (credit at dispatch), so
# a parked frame is credited early and torn/lost — credit_for_parked>0 (deterministic). The control
# binary exits 0 iff the defect fired (self-checking). Run under TSan too: the collapsed cursor also
# races the producer's overwrite vs the resume read on the parked slot (ADR-014 CONTROL-4 TSan teeth).
# CONTROL-5 (re-enqueue): -DQUARK_STREAM_REENQUEUE re-enqueues + resumes inline on the carrier instead
# of transferring, so a frame is double-dispatched (dup>0 / membership>1 / two executors).
#
# NEITHER control is wired under TSan. Both defects converge on a state the shared test detects with a
# no-progress SPIN watchdog: re-enqueue WEDGES the producer (double advance_tail corrupts unsigned
# credit -> push_blocking parks forever); single-cursor can LOSE frames (early credit -> producer
# overwrites the parked slot) so `processed` never reaches N. Either way the 1e9-iteration spin watchdog
# is far too slow under TSan's ~15x instrumentation and exceeds the ctest timeout (flaky/timeout) — the
# same reason stateless_pool_shared_state_control is excluded above. Their teeth are fully demonstrated
# in the gcc/clang/ASan builds (deterministic FIRE in seconds). The REAL (non-control)
# stream_exactly_once_suspend_test IS a first-class TSan test — it never wedges (it is correct), so it
# verifies the exactly-once-across-suspend path is race-free under ThreadSanitizer.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stream_exactly_once_suspend_test.cpp" AND NOT _quark_tsan)
  add_executable(stream_single_cursor_control stream_exactly_once_suspend_test.cpp)
  target_link_libraries(stream_single_cursor_control PRIVATE quark::quark quark_warnings Threads::Threads)
  target_compile_definitions(stream_single_cursor_control PRIVATE QUARK_STREAM_SINGLE_CURSOR)
  add_test(NAME stream_single_cursor_control COMMAND stream_single_cursor_control)

  add_executable(stream_reenqueue_control stream_exactly_once_suspend_test.cpp)
  target_link_libraries(stream_reenqueue_control PRIVATE quark::quark quark_warnings Threads::Threads)
  target_compile_definitions(stream_reenqueue_control PRIVATE QUARK_STREAM_REENQUEUE)
  add_test(NAME stream_reenqueue_control COMMAND stream_reenqueue_control)
endif()

# CONTROL-6 (fence removed): -DQUARK_STREAM_NO_FENCE drops the seq_cst close-out fence in the isolating
# Dekker litmus, so an arm racing the close-out is LOST (lost>0). The control exits 0 iff it stranded.
#
# NOT wired under ANY sanitizer. This is a PURE store-buffer-reorder litmus — the NO_FENCE variant
# removes ONLY the seq_cst StoreLoad fence; its "loss" is the x86-TSO store-past-load reorder (both
# threads read 0). ThreadSanitizer instruments every atomic load/store with a serializing call and does
# NOT model the store buffer, so the reorder cannot manifest under TSan. AddressSanitizer does not
# serialize atomics, but its per-access shadow-memory checks perturb instruction scheduling enough to
# drain the store buffer before the racing load, so the reorder likewise fails to manifest under ASan
# (observed: lost=0 over 1,000,000 trials). Under either sanitizer the control deterministically reports
# NOT-FIRED (lost=0, exit 1) — a false failure of the tool, not the code. (Contrast sched_no_lost_wakeup_
# control above, whose defect drops the RESCAN — a logic strand that fires under any memory model, so it
# IS a valid TSan test.) The POSITIVE stream_close_out_dekker_test (fence present, lost==0) is auto-
# discovered and DOES run under every sanitizer. The control's teeth are proven in the gcc/clang builds.
#
# x86-64 ONLY: the litmus's premise IS the x86-TSO store-past-load reorder specifically (see above) —
# there is no reason to expect the same removed-fence window reproduces at the same reliability (or at
# all) under AArch64's weaker model within the same trial budget; that needs its own arm64-calibrated
# litmus (see the TODO(arm64) notes in mailbox.hpp/engine.hpp/stream_channel.hpp), not a reuse of this
# one. Observed: NOT-FIRED on arm64 gcc/clang-release. Gate to x86-64 until that re-gate lands.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stream_close_out_dekker_test.cpp" AND NOT QUARK_SANITIZE
   AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64"))
  add_executable(stream_close_out_dekker_control stream_close_out_dekker_test.cpp)
  target_link_libraries(stream_close_out_dekker_control PRIVATE quark::quark quark_warnings Threads::Threads)
  target_compile_definitions(stream_close_out_dekker_control PRIVATE QUARK_STREAM_NO_FENCE)
  add_test(NAME stream_close_out_dekker_control COMMAND stream_close_out_dekker_control)
endif()

# ADR-028 Phase 1 CONTROL-1 (fence removed): -DQUARK_ACTIVATION_DEACTIVATE_NO_FENCE drops the seq_cst
# close-out fence in the isolating Dekker litmus, so a message racing the retire is LOST (lost>0). The
# control exits 0 iff it stranded. Same rationale/exclusions as stream_close_out_dekker_control above
# (pure store-buffer-reorder litmus: not meaningful under TSan/ASan, x86-64 only) — see that entry's
# comment for the full explanation; it applies identically here.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/activation_deactivate_close_out_dekker_test.cpp" AND NOT QUARK_SANITIZE
   AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64"))
  add_executable(activation_deactivate_close_out_dekker_control activation_deactivate_close_out_dekker_test.cpp)
  target_link_libraries(activation_deactivate_close_out_dekker_control PRIVATE quark::quark quark_warnings Threads::Threads)
  target_compile_definitions(activation_deactivate_close_out_dekker_control PRIVATE QUARK_ACTIVATION_DEACTIVATE_NO_FENCE)
  add_test(NAME activation_deactivate_close_out_dekker_control COMMAND activation_deactivate_close_out_dekker_control)
endif()

# 024-Streaming §0-cross-core-RMW-on-drain — objdump proof (ADR-005 S1 method). Disassemble the
# `quark_stream_drain_probe` symbol and grep for lock/cmpxchg/xadd/xchg-mem/mfence. Gated to
# NON-sanitizer builds (ASan/TSan/UBSan instrumentation injects lock ops that are not the drain's).
# NOT WIN32: objdump/bash are not part of the MSVC toolchain (the underlying stream_drain_rmw_test
# binary still builds and runs as a normal auto-discovered test above — only this extra disassembly
# proof is Linux/GNU-only).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stream_drain_rmw_test.cpp" AND NOT QUARK_SANITIZE AND NOT WIN32)
  add_test(NAME stream_drain_zero_rmw
           COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/stream_drain_rmw_check.sh" $<TARGET_FILE:stream_drain_rmw_test>)
endif()

# 006 §Publish/Subscribe — ADR-019 GATE 6 bounded-quiescence FIRING CONTROL. -DQUARK_TOPIC_NO_QUIESCE
# drops the `wait in_flight==0` barrier in Topic::unsubscribe, so the churn thread destroys an inbox a
# publisher still references (it loaded the pre-unsubscribe snapshot) -> heap-use-after-free. Wired
# ONLY under ASAN (where the freed-memory access reliably traps); WILL_FAIL means a non-zero exit (the
# trap) == the control firing. In a plain build the UAF may not crash, so the control has value only
# with ASan. The POSITIVE topic_subscribe_race_test (barrier present) is auto-discovered and runs under
# every build + sanitizer.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/topic_subscribe_race_test.cpp" AND QUARK_SANITIZE MATCHES "address")
  add_executable(topic_no_quiesce_control topic_subscribe_race_test.cpp)
  target_link_libraries(topic_no_quiesce_control PRIVATE quark::quark quark_warnings Threads::Threads)
  target_compile_definitions(topic_no_quiesce_control PRIVATE QUARK_TOPIC_NO_QUIESCE)
  add_test(NAME topic_no_quiesce_control COMMAND topic_no_quiesce_control)
  # ASan must exit with a clean non-zero code (not SIGABRT) so WILL_FAIL can invert it into a PASS;
  # symbolize=0 avoids the missing-llvm-symbolizer hang. WILL_FAIL => the ASan trap == the control firing.
  set_tests_properties(topic_no_quiesce_control PROPERTIES
    WILL_FAIL TRUE
    ENVIRONMENT "ASAN_OPTIONS=abort_on_error=0:symbolize=0:exitcode=1;UBSAN_OPTIONS=print_stacktrace=0")
endif()
