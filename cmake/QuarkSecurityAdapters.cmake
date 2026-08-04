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
  find_package(MbedTLS CONFIG QUIET)
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
