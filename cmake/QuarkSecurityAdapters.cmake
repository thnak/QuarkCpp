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
# Never copy a prebuilt into this repository; point at it. A STATIC MSVC prebuilt built /MT (the libsodium release zip)
# linked into a /MD build needs -DQUARK_SODIUM_STATIC=ON, which defines SODIUM_STATIC and adds /NODEFAULTLIB:libcmt.lib.
# =============================================================================================================
option(QUARK_WITH_SODIUM "Build the libsodium pinned-key adapter (Noise IK + ChaCha20-Poly1305, ADR-068)" OFF)
option(QUARK_SODIUM_STATIC "libsodium is a static library (defines SODIUM_STATIC; MSVC: /NODEFAULTLIB:libcmt.lib)" OFF)
set(QUARK_SODIUM_INCLUDE_DIR "" CACHE PATH "Directory containing sodium.h (non-vcpkg builds)")
set(QUARK_SODIUM_LIBRARY "" CACHE FILEPATH "libsodium library file (non-vcpkg builds)")

if(QUARK_WITH_SODIUM)
  add_library(quark_security_sodium INTERFACE)
  find_package(unofficial-sodium CONFIG QUIET)
  if(QUARK_SODIUM_INCLUDE_DIR AND QUARK_SODIUM_LIBRARY)
    target_include_directories(quark_security_sodium INTERFACE "${QUARK_SODIUM_INCLUDE_DIR}")
    target_link_libraries(quark_security_sodium INTERFACE quark::quark "${QUARK_SODIUM_LIBRARY}")
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
      target_link_options(quark_security_sodium INTERFACE "/NODEFAULTLIB:libcmt.lib")
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
               pinned_secure_transport_check pinned_tcp_check pinned_flood_check)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/${_chk}.cpp")
      add_executable(${_chk} tests/adapters/${_chk}.cpp)
      target_include_directories(${_chk} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
      target_compile_definitions(${_chk} PRIVATE QUARK_ALLOW_INSECURE
        QUARK_TEST_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors")
      target_link_libraries(${_chk} PRIVATE quark::security_sodium quark_warnings Threads::Threads)
      add_test(NAME ${_chk} COMMAND ${_chk})
    endif()
  endforeach()
  # Cross-mode (TLS CA chain <-> pinned): needs BOTH adapters in one binary.
  if(QUARK_WITH_MBEDTLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/adapters/pinned_crossmode_check.cpp")
    add_executable(pinned_crossmode_check tests/adapters/pinned_crossmode_check.cpp)
    target_include_directories(pinned_crossmode_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(pinned_crossmode_check PRIVATE quark::security_sodium quark::security_mbedtls
                                                         quark_warnings Threads::Threads)
    add_test(NAME pinned_crossmode_check COMMAND pinned_crossmode_check)
  endif()
endif()
