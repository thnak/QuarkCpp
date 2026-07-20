cmake_minimum_required(VERSION 3.24)
project(Quark LANGUAGES CXX VERSION 0.1.0
        DESCRIPTION "Quark — high-performance C++ actor engine (Linux/x86-64, CI-verified Linux/arm64)")

# ---- Standard --------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Primary target guard (CONVENTIONS.md). Linux/x86-64 is the reference platform (023 perf budgets
# are pinned to it); Linux/arm64 is CI-verified (the full correctness matrix runs on real AArch64
# hardware, .github/workflows/ci.yml) but not yet spec-Accepted — the formal weak-memory-model
# re-gate of the TODO(arm64) seams and a 023 arm64 re-baseline are still open. Anything else
# (Windows/macOS, other arches) has no PAL backend yet.
if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|aarch64|arm64"))
  message(WARNING "Quark is verified on Linux/x86-64 and CI-verified on Linux/arm64; this is "
                  "${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}. Other platforms need their PAL "
                  "backend (spec 019).")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(QuarkWarnings)

# ---- Core library ----------------------------------------------------------
# Header-first core (templates/hot path live in headers); src/ holds non-template units.
add_library(quark STATIC)
target_sources(quark PRIVATE src/version.cpp)
target_include_directories(quark PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}")   # for pal/pal.hpp
target_compile_features(quark PUBLIC cxx_std_23)
target_link_libraries(quark PRIVATE quark_warnings)
add_library(quark::quark ALIAS quark)

# ---- Tests, benchmarks & samples -------------------------------------------
option(QUARK_BUILD_TESTS "Build Quark tests" ON)
option(QUARK_BUILD_BENCH "Build Quark benchmarks" ON)
# Samples default OFF so the test/sanitizer builds stay lean; enable with -DQUARK_BUILD_SAMPLES=ON.
option(QUARK_BUILD_SAMPLES "Build Quark sample projects" OFF)

if(QUARK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Optional 012 persistence backends (SQLite / RocksDB). OFF by default — the std-only core + the
# header-only FileStore need nothing here. `include()` after tests so it can register conformance
# checks against `enable_testing()`.
include(QuarkPersistenceAdapters)
if(QUARK_BUILD_BENCH)
  add_subdirectory(bench)
endif()
if(QUARK_BUILD_SAMPLES)
  add_subdirectory(samples)
endif()
