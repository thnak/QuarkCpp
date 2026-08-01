cmake_minimum_required(VERSION 3.24)
project(quark_caf_comparison LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_BUILD_TYPE Release)

find_package(Threads REQUIRED)

# --- Quark (requires C++23) -------------------------------------------------
set(QUARK_ROOT "D:/GitSrc/QuarkCpp")
set(QUARK_BUILD "${QUARK_ROOT}/build")
set(QUARK_LIB "${QUARK_BUILD}/quark.lib")

# --- CAF (C++20) -----------------------------------------------------------
set(CAF_DIR "C:/Users/thanh/AppData/Local/Temp/actor-framework")
set(CAF_CORE_DIR "${CAF_DIR}/libcaf_core")
set(CAF_LIB "${CAF_DIR}/build/libcaf_core/caf_core.lib")

# --- Quark bench (C++23) ----------------------------------------------------
add_executable(quark_bench quark_bench.cpp)
set_target_properties(quark_bench PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(quark_bench PRIVATE
    "${QUARK_ROOT}"
    "${QUARK_ROOT}/include"
)
target_link_libraries(quark_bench PRIVATE "${QUARK_LIB}" Threads::Threads)
if(WIN32)
    target_link_libraries(quark_bench PRIVATE ws2_32 bcrypt mincore)
    target_compile_definitions(quark_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# --- CAF bench (C++20) ------------------------------------------------------
add_executable(caf_bench caf_bench.cpp)
set_target_properties(caf_bench PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(caf_bench PRIVATE
    "${CAF_CORE_DIR}"
    "${CAF_DIR}/build/libcaf_core"  # generated headers (core_export.hpp)
)
target_link_libraries(caf_bench PRIVATE "${CAF_LIB}" Threads::Threads)
if(WIN32)
    target_compile_definitions(caf_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# --- Quark MPSC bench (C++23) -----------------------------------------------
add_executable(quark_mpsc_bench quark_mpsc_bench.cpp)
set_target_properties(quark_mpsc_bench PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(quark_mpsc_bench PRIVATE
    "${QUARK_ROOT}"
    "${QUARK_ROOT}/include"
)
target_link_libraries(quark_mpsc_bench PRIVATE "${QUARK_LIB}" Threads::Threads)
if(WIN32)
    target_link_libraries(quark_mpsc_bench PRIVATE ws2_32 bcrypt mincore)
    target_compile_definitions(quark_mpsc_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# --- CAF MPSC bench (C++20) --------------------------------------------------
add_executable(caf_mpsc_bench caf_mpsc_bench.cpp)
set_target_properties(caf_mpsc_bench PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(caf_mpsc_bench PRIVATE
    "${CAF_CORE_DIR}"
    "${CAF_DIR}/build/libcaf_core"
)
target_link_libraries(caf_mpsc_bench PRIVATE "${CAF_LIB}" Threads::Threads)
if(WIN32)
    target_compile_definitions(caf_mpsc_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# --- Quark sharded MPSC bench (C++23) — N producers -> N actors, contention-free ceiling -----
add_executable(quark_mpsc_sharded_bench quark_mpsc_sharded_bench.cpp)
set_target_properties(quark_mpsc_sharded_bench PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(quark_mpsc_sharded_bench PRIVATE
    "${QUARK_ROOT}"
    "${QUARK_ROOT}/include"
)
target_link_libraries(quark_mpsc_sharded_bench PRIVATE "${QUARK_LIB}" Threads::Threads)
if(WIN32)
    target_link_libraries(quark_mpsc_sharded_bench PRIVATE ws2_32 bcrypt mincore)
    target_compile_definitions(quark_mpsc_sharded_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# --- CAF sharded MPSC bench (C++20) -------------------------------------------
add_executable(caf_mpsc_sharded_bench caf_mpsc_sharded_bench.cpp)
set_target_properties(caf_mpsc_sharded_bench PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(caf_mpsc_sharded_bench PRIVATE
    "${CAF_CORE_DIR}"
    "${CAF_DIR}/build/libcaf_core"
)
target_link_libraries(caf_mpsc_sharded_bench PRIVATE "${CAF_LIB}" Threads::Threads)
if(WIN32)
    target_compile_definitions(caf_mpsc_sharded_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# --- Quark sustained stress/survivability test (C++23) — M producers -> M actors (paired
# lanes), fixed wall-clock duration instead of fixed op count; see file header for rationale. ---
add_executable(quark_stress_bench quark_stress_bench.cpp)
set_target_properties(quark_stress_bench PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(quark_stress_bench PRIVATE
    "${QUARK_ROOT}"
    "${QUARK_ROOT}/include"
)
target_link_libraries(quark_stress_bench PRIVATE "${QUARK_LIB}" Threads::Threads)
if(WIN32)
    target_link_libraries(quark_stress_bench PRIVATE ws2_32 bcrypt mincore)
    target_compile_definitions(quark_stress_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# --- CAF sustained stress/survivability test (C++20) — mirrors quark_stress_bench.cpp exactly
# (same paired M:S topology, same safety design) for a direct comparison. ------------------------
add_executable(caf_stress_bench caf_stress_bench.cpp)
set_target_properties(caf_stress_bench PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(caf_stress_bench PRIVATE
    "${CAF_CORE_DIR}"
    "${CAF_DIR}/build/libcaf_core"
)
target_link_libraries(caf_stress_bench PRIVATE "${CAF_LIB}" Threads::Threads)
if(WIN32)
    target_compile_definitions(caf_stress_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
