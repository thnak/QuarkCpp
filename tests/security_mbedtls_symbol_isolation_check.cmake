# Implements ADR-040 Phase 7 (imports Design 1's C8) — the actual build-isolation PROOF. Invoked as a
# CTest COMMAND (see tests/CMakeLists.txt) with -DDUMPBIN_EXE=<path to dumpbin.exe> and
# -DTARGET_LIB=<path to the built quark.lib>. Fails (message(FATAL_ERROR) -> non-zero exit -> CTest
# FAIL) if any mbedtls_-prefixed symbol appears in the DEFAULT (QUARK_WITH_MBEDTLS=OFF) build's core
# static library — the one artifact every default build produces and every consumer links against, so a
# clean scan here is a real end-to-end isolation proof, not just a header-inclusion sanity check.
if(NOT EXISTS "${TARGET_LIB}")
    message(FATAL_ERROR "security_mbedtls_symbol_isolation: TARGET_LIB does not exist: ${TARGET_LIB}")
endif()

execute_process(
    COMMAND "${DUMPBIN_EXE}" /symbols "${TARGET_LIB}"
    OUTPUT_VARIABLE _quark_dumpbin_out
    ERROR_VARIABLE _quark_dumpbin_err
    RESULT_VARIABLE _quark_dumpbin_rc
)
if(NOT _quark_dumpbin_rc EQUAL 0)
    message(FATAL_ERROR
        "security_mbedtls_symbol_isolation: dumpbin failed (rc=${_quark_dumpbin_rc}): ${_quark_dumpbin_err}")
endif()

string(TOLOWER "${_quark_dumpbin_out}" _quark_dumpbin_out_lower)
string(FIND "${_quark_dumpbin_out_lower}" "mbedtls_" _quark_hit)
if(NOT _quark_hit EQUAL -1)
    message(FATAL_ERROR
        "security_mbedtls_symbol_isolation: FOUND an mbedtls_-prefixed symbol in the DEFAULT "
        "(QUARK_WITH_MBEDTLS=OFF) build of ${TARGET_LIB} -- the core is no longer isolated from the "
        "optional mTLS adapter (ADR-040). Check for an accidental #include of "
        "quark/adapters/mbedtls/*.hpp from a CORE header, or a link-order mistake pulling "
        "quark_security_mbedtls into the default `quark` target.")
endif()

message(STATUS "security_mbedtls_symbol_isolation: OK -- 0 mbedtls_ symbols in ${TARGET_LIB}")
