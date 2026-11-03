# Optional 020 production transport-security backend (mbedTLS). OFF by default so the std-only
# core build links nothing extra — per 020-Security.md's "honest exception" (crypto is the one
# deliberate exception to the std-only-core rule, but it stays an opt-in adapter like every other
# heavy dependency): the built-in `MockCipher`/no-op handshake remain the default, dev/loopback-only
# path; a real mTLS handshake + AES-GCM AEAD are only linked when a secure cluster is configured.
# ADR-040 settled mbedTLS over BoringSSL for this project's Windows/MSVC-primary toolchain (no
# stable ABI / Bazel-only build path disqualified BoringSSL).
#
# Enable with:  cmake -B build -DQUARK_WITH_MBEDTLS=ON \
#                 -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
# (vcpkg.json at the repo root pins the mbedTLS version; manifest mode installs it automatically
# on first configure.) A non-vcpkg mbedTLS install (system package, manual build) also works via
# the find_path/find_library fallback below.

option(QUARK_WITH_MBEDTLS "Build the mbedTLS transport-security adapter (real mTLS + AES-GCM, ADR-040)" OFF)

if(QUARK_WITH_MBEDTLS AND QUARK_BUILD_TESTS)
  find_package(Threads REQUIRED)
endif()

if(QUARK_WITH_MBEDTLS)
  # Prefer the packaged CMake config (vcpkg ships one); fall back to a plain library/header search
  # for a system-installed mbedTLS. Mirrors cmake/QuarkPersistenceAdapters.cmake's RocksDB branch.
  find_package(MbedTLS 3.6.2 CONFIG QUIET)
  add_library(quark_security_mbedtls INTERFACE)
  if(TARGET MbedTLS::mbedtls)
    target_link_libraries(quark_security_mbedtls INTERFACE
      quark::quark MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  else()
    find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h)
    find_library(MBEDTLS_LIBRARY        NAMES mbedtls)
    find_library(MBEDX509_LIBRARY       NAMES mbedx509)
    find_library(MBEDCRYPTO_LIBRARY     NAMES mbedcrypto)
    if(NOT MBEDTLS_INCLUDE_DIR OR NOT MBEDTLS_LIBRARY OR NOT MBEDX509_LIBRARY OR NOT MBEDCRYPTO_LIBRARY)
      message(FATAL_ERROR "QUARK_WITH_MBEDTLS=ON but mbedTLS not found. "
                          "Configure with -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake "
                          "(vcpkg.json pins the version), or install mbedTLS and set "
                          "MBEDTLS_INCLUDE_DIR/MBEDTLS_LIBRARY/MBEDX509_LIBRARY/MBEDCRYPTO_LIBRARY.")
    endif()
    target_include_directories(quark_security_mbedtls INTERFACE "${MBEDTLS_INCLUDE_DIR}")
    target_link_libraries(quark_security_mbedtls INTERFACE
      quark::quark "${MBEDTLS_LIBRARY}" "${MBEDX509_LIBRARY}" "${MBEDCRYPTO_LIBRARY}")
  endif()
  add_library(quark::security_mbedtls ALIAS quark_security_mbedtls)
  message(STATUS "Quark: mbedTLS transport-security adapter ENABLED")
endif()

# ---- Conformance / correctness checks (only when tests are on AND the backend is enabled) -----
# tests/adapters/mbedtls_aead_check.cpp: Aead conformance (seal/open/seal_into/open_into) against
# the shared aead_conformance.hpp harness used by MockCipher, run against the real AES-128-GCM impl.
# tests/adapters/mbedtls_handshake_check.cpp: real loopback TLS1.3 mutual-handshake round trips
# (glare-free role assignment, mismatched-CA rejection, revoked-fingerprint rejection) — see ADR-040
# Design 2's C1/C4/C11/S3 evidence, reproduced here against this codebase's own adapter.
if(QUARK_BUILD_TESTS AND QUARK_WITH_MBEDTLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/mbedtls_aead_check.cpp")
  add_executable(mbedtls_aead_check tests/adapters/mbedtls_aead_check.cpp)
  target_include_directories(mbedtls_aead_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(mbedtls_aead_check PRIVATE quark::security_mbedtls quark_warnings)
  add_test(NAME mbedtls_aead_check COMMAND mbedtls_aead_check)
endif()
if(QUARK_BUILD_TESTS AND QUARK_WITH_MBEDTLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/mbedtls_handshake_check.cpp")
  add_executable(mbedtls_handshake_check tests/adapters/mbedtls_handshake_check.cpp)
  target_include_directories(mbedtls_handshake_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(mbedtls_handshake_check PRIVATE quark::security_mbedtls quark_warnings Threads::Threads)
  add_test(NAME mbedtls_handshake_check COMMAND mbedtls_handshake_check)
endif()
# tests/adapters/mbedtls_enrollment_check.cpp: ADR-074 D4/C5 — MbedtlsEnrolCrypto (HMAC, CSR proof-of-possession,
# issued-certificate rules), the joiner key/CSR/MAC helpers, Enrollment end to end, the C5 startup check.
if(QUARK_BUILD_TESTS AND QUARK_WITH_MBEDTLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/mbedtls_enrollment_check.cpp")
  add_executable(mbedtls_enrollment_check tests/adapters/mbedtls_enrollment_check.cpp)
  target_include_directories(mbedtls_enrollment_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(mbedtls_enrollment_check PRIVATE quark::security_mbedtls quark_warnings Threads::Threads)
  add_test(NAME mbedtls_enrollment_check COMMAND mbedtls_enrollment_check)
endif()
# tests/adapters/mbedtls_enrol_channel_check.cpp: ADR-074 D4c � the server-authenticated channel an enrolment travels in (the joiner
# trusts only the join code's CA pin; client certificate, wrong cluster/node, rogue CA, pin mismatch all refused).
if(QUARK_BUILD_TESTS AND QUARK_WITH_MBEDTLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/mbedtls_enrol_channel_check.cpp")
  add_executable(mbedtls_enrol_channel_check tests/adapters/mbedtls_enrol_channel_check.cpp)
  target_include_directories(mbedtls_enrol_channel_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(mbedtls_enrol_channel_check PRIVATE quark::security_mbedtls quark_warnings Threads::Threads)
  add_test(NAME mbedtls_enrol_channel_check COMMAND mbedtls_enrol_channel_check)
endif()

# tests/adapters/mbedtls_enrol_session_check.cpp: ADR-074 D4c step 3 - one whole enrolment in memory (anchor session over a real Enrollment, joiner session
# that verifies what it is issued itself).
if(QUARK_BUILD_TESTS AND QUARK_WITH_MBEDTLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/mbedtls_enrol_session_check.cpp")
  add_executable(mbedtls_enrol_session_check tests/adapters/mbedtls_enrol_session_check.cpp)
  target_include_directories(mbedtls_enrol_session_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(mbedtls_enrol_session_check PRIVATE quark::security_mbedtls quark_warnings Threads::Threads)
  add_test(NAME mbedtls_enrol_session_check COMMAND mbedtls_enrol_session_check)
endif()

# tests/adapters/mbedtls_enrol_service_check.cpp: ADR-074 D4c step 4 - enrolment over a real TcpTransport listener (handoff, caps, no ticket, off the I/O thread).
if(QUARK_BUILD_TESTS AND QUARK_WITH_MBEDTLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/mbedtls_enrol_service_check.cpp")
  add_executable(mbedtls_enrol_service_check tests/adapters/mbedtls_enrol_service_check.cpp)
  target_include_directories(mbedtls_enrol_service_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_compile_definitions(mbedtls_enrol_service_check PRIVATE QUARK_ALLOW_INSECURE)  # F1: the member listener is a dev plaintext TcpTransport here
  target_link_libraries(mbedtls_enrol_service_check PRIVATE quark::quark quark::security_mbedtls quark_warnings Threads::Threads)
  add_test(NAME mbedtls_enrol_service_check COMMAND mbedtls_enrol_service_check)
endif()

# tests/adapters/mbedtls_enrol_e2e_check.cpp: ADR-074 D4c / #36 - a node with only a join code enrols and then joins as a member over TcpTransport + SecureTransport.
if(QUARK_BUILD_TESTS AND QUARK_WITH_MBEDTLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/mbedtls_enrol_e2e_check.cpp")
  add_executable(mbedtls_enrol_e2e_check tests/adapters/mbedtls_enrol_e2e_check.cpp)
  target_include_directories(mbedtls_enrol_e2e_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(mbedtls_enrol_e2e_check PRIVATE quark::quark quark::security_mbedtls quark_warnings Threads::Threads)
  add_test(NAME mbedtls_enrol_e2e_check COMMAND mbedtls_enrol_e2e_check)
endif()

# =============================================================================================================
# Optional pinned-key transport-security backend (libsodium; Noise IK, issue #40 / ADR-068). OFF by default so the
# std-only core build links nothing extra — the same posture as QUARK_WITH_MBEDTLS above (020 "honest exception":
# crypto is an opt-in adapter over a vetted library, never in the core). This is the SECOND crypto adapter. A
# pinned-mode build needs libsodium ONLY; mbedTLS is independent (the two may be enabled together, which the
# cross-mode check requires).
#
# Enable with:  cmake -B build -DQUARK_WITH_SODIUM=ON -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
# (vcpkg.json declares libsodium), or against a system libsodium (apt: libsodium-dev), or an explicit prebuilt via
#   -DQUARK_SODIUM_INCLUDE_DIR=<dir containing sodium.h> -DQUARK_SODIUM_LIBRARY=<path to libsodium.lib / .a / .so>
#   [-DQUARK_SODIUM_LIBRARY_DEBUG=<path to the Debug library>]
# Never copy a prebuilt into this repository; point at it. A STATIC MSVC prebuilt built /MT (the libsodium release zip)
# linked into a /MD build needs -DQUARK_SODIUM_STATIC=ON, which defines SODIUM_STATIC and suppresses the matching static
# CRT: libcmt in Release-like configurations, libcmtd in Debug. The Windows zip ships a Debug library built /MTd, so give
# it its own path with QUARK_SODIUM_LIBRARY_DEBUG; otherwise the single QUARK_SODIUM_LIBRARY is used everywhere (#82).
# =============================================================================================================
option(QUARK_WITH_SODIUM "Build the libsodium pinned-key adapter (Noise IK + ChaCha20-Poly1305, ADR-068)" OFF)
option(QUARK_SODIUM_STATIC "libsodium is a static library (defines SODIUM_STATIC; MSVC: suppresses the static CRT)" OFF)
set(QUARK_SODIUM_INCLUDE_DIR "" CACHE PATH "Directory containing sodium.h (non-vcpkg builds)")
set(QUARK_SODIUM_LIBRARY "" CACHE FILEPATH "libsodium library file (non-vcpkg builds)")
set(QUARK_SODIUM_LIBRARY_DEBUG "" CACHE FILEPATH
    "Debug-config libsodium library file (non-vcpkg builds; defaults to QUARK_SODIUM_LIBRARY)")

if(QUARK_WITH_SODIUM)
  add_library(quark_security_sodium INTERFACE)
  find_package(unofficial-sodium CONFIG QUIET)
  if(QUARK_SODIUM_INCLUDE_DIR AND (QUARK_SODIUM_LIBRARY OR QUARK_SODIUM_LIBRARY_DEBUG))
    target_include_directories(quark_security_sodium INTERFACE "${QUARK_SODIUM_INCLUDE_DIR}")
    # #82: link the configuration's own prebuilt. The libsodium Windows zip ships a /MT Release library and a
    # /MTd Debug one, so a /MDd build must not be handed the Release path. A missing Debug path falls back to
    # the single QUARK_SODIUM_LIBRARY; a Debug-only setup links its path in every configuration.
    set(_quark_sodium_release_lib "${QUARK_SODIUM_LIBRARY}")
    if(NOT _quark_sodium_release_lib)
      set(_quark_sodium_release_lib "${QUARK_SODIUM_LIBRARY_DEBUG}")
    endif()
    set(_quark_sodium_debug_lib "${QUARK_SODIUM_LIBRARY_DEBUG}")
    if(NOT _quark_sodium_debug_lib)
      set(_quark_sodium_debug_lib "${_quark_sodium_release_lib}")
    endif()
    if(_quark_sodium_debug_lib STREQUAL _quark_sodium_release_lib)
      target_link_libraries(quark_security_sodium INTERFACE quark::quark "${_quark_sodium_release_lib}")
    else()
      target_link_libraries(quark_security_sodium INTERFACE quark::quark
        "$<$<CONFIG:Debug>:${_quark_sodium_debug_lib}>"
        "$<$<NOT:$<CONFIG:Debug>>:${_quark_sodium_release_lib}>")
    endif()
  elseif(TARGET unofficial-sodium::sodium)
    target_link_libraries(quark_security_sodium INTERFACE quark::quark unofficial-sodium::sodium)
  else()
    find_path(QUARK_SODIUM_FOUND_INCLUDE sodium.h)
    find_library(QUARK_SODIUM_FOUND_LIBRARY NAMES sodium libsodium)
    if(NOT QUARK_SODIUM_FOUND_INCLUDE OR NOT QUARK_SODIUM_FOUND_LIBRARY)
      message(FATAL_ERROR "QUARK_WITH_SODIUM=ON but libsodium not found. Configure with "
                          "-DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake (vcpkg.json declares it), install "
                          "libsodium-dev, or set QUARK_SODIUM_INCLUDE_DIR and QUARK_SODIUM_LIBRARY.")
    endif()
    target_include_directories(quark_security_sodium INTERFACE "${QUARK_SODIUM_FOUND_INCLUDE}")
    target_link_libraries(quark_security_sodium INTERFACE quark::quark "${QUARK_SODIUM_FOUND_LIBRARY}")
  endif()
  if(QUARK_SODIUM_STATIC)
    target_compile_definitions(quark_security_sodium INTERFACE SODIUM_STATIC=1)
    if(MSVC)
      # #82: the static prebuilt's Debug library is /MTd, so a /MDd build suppresses libcmtd; Release-like
      # configurations keep suppressing libcmt (the release zip is /MT).
      target_link_options(quark_security_sodium INTERFACE
        "$<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>"
        "$<$<NOT:$<CONFIG:Debug>>:/NODEFAULTLIB:libcmt.lib>")
    endif()
  endif()
  add_library(quark::security_sodium ALIAS quark_security_sodium)
  # Issue #50: the fuzz targets for the pinned Noise engine compile only with libsodium. The drivers are created in
  # tests/ (before this module runs), so wire the dependency here; without sodium the targets compile out cleanly.
  foreach(_fz fuzz_decoders_test fuzz_decoders fuzz_seed_corpus fuzz_pinned_oob_control)
    if(TARGET ${_fz})
      target_link_libraries(${_fz} PRIVATE quark::security_sodium)
      target_compile_definitions(${_fz} PRIVATE QUARK_FUZZ_SODIUM=1)
    endif()
  endforeach()
  message(STATUS "Quark: libsodium pinned-key adapter ENABLED")
endif()

# Checks (tests/adapters/*_check.cpp, standalone executables like the mbedTLS ones; each property has a control that
# must fail). Registered only when tests are on AND the backend is enabled.
if(QUARK_BUILD_TESTS AND QUARK_WITH_SODIUM)
  find_package(Threads REQUIRED)
  foreach(_chk noise_ik_vectors_check sodium_aead_check pinned_handshake_check pinned_privacy_check
               pinned_secure_transport_check pinned_tcp_check pinned_flood_check
               pinned_nodeid_role_check pinned_key_copies_check pinned_timing_oracle_check
               pinned_reconnect_newip_check pinned_client_library_check)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/${_chk}.cpp")
      add_executable(${_chk} tests/adapters/${_chk}.cpp)
      target_include_directories(${_chk} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
      target_compile_definitions(${_chk} PRIVATE QUARK_ALLOW_INSECURE
        QUARK_TEST_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors")
      target_link_libraries(${_chk} PRIVATE quark::security_sodium quark_warnings Threads::Threads)
      if(_chk STREQUAL "pinned_timing_oracle_check")
        # Slow measurement (#69): registered under a name the CI regex "noise_ik|sodium_aead|pinned_|security_sodium" skips.
        add_test(NAME timing_oracle_pinned COMMAND ${_chk})
        set_tests_properties(timing_oracle_pinned PROPERTIES LABELS "timing")
      else()
        add_test(NAME ${_chk} COMMAND ${_chk})
      endif()
    endif()
  endforeach()
  # #64 step 2 CONTROL: the same source with the pre-fix by-value KeyPair heap copy restored. The heap scan MUST find the
  # marker, so this target is registered WILL_FAIL — a control that stops demonstrating the leak fails the build.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/pinned_key_copies_check.cpp")
    add_executable(pinned_key_copies_check_control tests/adapters/pinned_key_copies_check.cpp)
    target_include_directories(pinned_key_copies_check_control PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(pinned_key_copies_check_control PRIVATE QUARK_ALLOW_INSECURE
      QUARK_TEST_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors" QUARK_PINNED_KEY_COPIES_CONTROL=1)
    target_link_libraries(pinned_key_copies_check_control PRIVATE quark::security_sodium quark_warnings Threads::Threads)
    add_test(NAME pinned_key_copies_check_control COMMAND pinned_key_copies_check_control)
    set_tests_properties(pinned_key_copies_check_control PROPERTIES WILL_FAIL TRUE)
  endif()

  # #64: the instrumented allocator lives in its own TU (tests/adapters/heap_probe.hpp explains why) so GCC does not inline
  # the replacement's free() into a caller's new-expression and report -Wmismatched-new-delete.
  foreach(_probed pinned_key_copies_check pinned_key_copies_check_control)
    if(TARGET ${_probed})
      target_sources(${_probed} PRIVATE tests/adapters/heap_probe.cpp)
    endif()
  endforeach()

  # #51 (assessment M6): differential test of the Noise IK glue against an independent implementation (the Python `noiseprotocol`
  # package). The C++ tool is always built; the tests are registered only when Python 3 with that package is found, and under names
  # the CI regex "noise_ik|sodium_aead|pinned_|security_sodium" does not match. CONTROLS: copies of noise.hpp with ONE line changed
  # in MixHash / MixKey, built into their own tools, which the driver must report as DIFFERENT (--expect-mismatch).
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/noise_ik_differential_tool.cpp")
    add_executable(noise_ik_differential_tool tests/adapters/noise_ik_differential_tool.cpp)
    target_link_libraries(noise_ik_differential_tool PRIVATE quark::security_sodium quark_warnings)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/quark/adapters/sodium/noise.hpp" _noise_src)
    set(_mut_hash_from "void mix_hash(std::span<const std::byte> data) noexcept { h_ = hash(h_, data); }")
    set(_mut_hash_to   "void mix_hash(std::span<const std::byte> data) noexcept { h_ = hash(data, h_); }")
    set(_mut_key_from  "std::memcpy(k.data(), temp_k.data(), kKeyLen);")
    set(_mut_key_to    "std::memcpy(k.data(), temp_k.data() + 1, kKeyLen);")
    foreach(_mut hash key)
      string(REPLACE "${_mut_${_mut}_from}" "${_mut_${_mut}_to}" _mutated "${_noise_src}")
      if(_mutated STREQUAL _noise_src)  # the control must really differ, or it proves nothing
        message(FATAL_ERROR "noise_ik_differential: the mix_${_mut} mutation did not apply (noise.hpp changed?)")
      endif()
      set(_mdir "${CMAKE_BINARY_DIR}/noise_ik_mutant_${_mut}")
      file(WRITE "${_mdir}/quark/adapters/sodium/noise.hpp" "${_mutated}")
      add_executable(noise_ik_differential_tool_mut_${_mut} tests/adapters/noise_ik_differential_tool.cpp)
      target_include_directories(noise_ik_differential_tool_mut_${_mut} BEFORE PRIVATE "${_mdir}")
      target_link_libraries(noise_ik_differential_tool_mut_${_mut} PRIVATE quark::security_sodium quark_warnings)
    endforeach()
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import noise.connection, cryptography"
                      RESULT_VARIABLE _noise_py_rc OUTPUT_QUIET ERROR_QUIET)
      if(_noise_py_rc EQUAL 0)
        set(_diff_py "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/noise_ik_differential.py")
        add_test(NAME differential_noise_ik COMMAND "${Python3_EXECUTABLE}" "${_diff_py}" --tool $<TARGET_FILE:noise_ik_differential_tool>)
        foreach(_mut hash key)
          add_test(NAME differential_noise_ik_control_${_mut}
                   COMMAND "${Python3_EXECUTABLE}" "${_diff_py}" --tool $<TARGET_FILE:noise_ik_differential_tool_mut_${_mut}>
                           --expect-mismatch --n 200)
        endforeach()
        set_tests_properties(differential_noise_ik differential_noise_ik_control_hash differential_noise_ik_control_key
                             PROPERTIES LABELS "differential")
        message(STATUS "Quark: Noise IK differential tests ENABLED (noiseprotocol found)")
      else()
        message(STATUS "Quark: Noise IK differential tests skipped (pip install noiseprotocol cryptography)")
      endif()
    endif()
  endif()

  # Cross-mode (TLS CA chain <-> pinned): needs BOTH adapters in one binary.
  if(QUARK_WITH_MBEDTLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/pinned_crossmode_check.cpp")
    add_executable(pinned_crossmode_check tests/adapters/pinned_crossmode_check.cpp)
    target_include_directories(pinned_crossmode_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(pinned_crossmode_check PRIVATE quark::security_sodium quark::security_mbedtls
                                                         quark_warnings Threads::Threads)
    add_test(NAME pinned_crossmode_check COMMAND pinned_crossmode_check)
  endif()
endif()
