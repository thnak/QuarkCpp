# Implements ADR-040 Phase 7 (imports Design 1's C8) — the actual build-isolation PROOF. Invoked as a
# CTest COMMAND (see tests/CMakeLists.txt) with -DDUMPBIN_EXE=<path to dumpbin.exe> and
# -DTARGET_LIB=<path to the built quark.lib>. Fails (message(FATAL_ERROR) -> non-zero exit -> CTest
# FAIL) if any mbedtls_-prefixed symbol appears in the DEFAULT (QUARK_WITH_MBEDTLS=OFF) build's core
# static library — the one artifact every default build produces and every consumer links against, so a
# clean scan here is a real end-to-end isolation proof, not just a header-inclusion sanity check.
if(NOT EXISTS "${TARGET_LIB}")
    message(FATAL_ERROR "security_mbedtls_symbol_isolation: TARGET_LIB does not exist: ${TARGET_LIB}")
endif()

# Symbol lister: MSVC `dumpbin /symbols` (DUMPBIN_EXE) or the GNU/LLVM `nm` (NM_EXE; Linux, issue #49). nm lists
# defined AND undefined symbols of every member of the archive, so a reference to a sodium_/crypto_ symbol counts too.
if(DEFINED NM_EXE)
    set(_quark_lister_cmd "${NM_EXE}" "${TARGET_LIB}")
elseif(DEFINED DUMPBIN_EXE)
    set(_quark_lister_cmd "${DUMPBIN_EXE}" /symbols "${TARGET_LIB}")
else()
    message(FATAL_ERROR "security_symbol_isolation: neither DUMPBIN_EXE nor NM_EXE given")
endif()
execute_process(
    COMMAND ${_quark_lister_cmd}
    OUTPUT_VARIABLE _quark_dumpbin_out
    ERROR_VARIABLE _quark_dumpbin_err
    RESULT_VARIABLE _quark_dumpbin_rc
)
if(NOT _quark_dumpbin_rc EQUAL 0)
    message(FATAL_ERROR
        "security_symbol_isolation: symbol lister failed (rc=${_quark_dumpbin_rc}): ${_quark_dumpbin_err}")
endif()

string(TOLOWER "${_quark_dumpbin_out}" _quark_dumpbin_out_lower)
# SYMBOL_PREFIXES (a |- or ;-list, default just "mbedtls_"): every one must be ABSENT. The sodium variant passes
# "sodium_|crypto_" (issue #40 / ADR-068: libsodium's public symbols are sodium_* and crypto_*).
if(NOT DEFINED SYMBOL_PREFIXES)
    set(SYMBOL_PREFIXES "mbedtls_")
endif()
string(REPLACE "|" ";" SYMBOL_PREFIXES "${SYMBOL_PREFIXES}")  # '|' survives add_test quoting; ';' does not
foreach(_prefix IN LISTS SYMBOL_PREFIXES)
    string(FIND "${_quark_dumpbin_out_lower}" "${_prefix}" _quark_hit)
    if(NOT _quark_hit EQUAL -1)
        message(FATAL_ERROR
            "security_symbol_isolation: FOUND a ${_prefix}-prefixed symbol in the core static library ${TARGET_LIB} -- "
            "the core is no longer isolated from the optional security adapters (ADR-040 / ADR-068). Check for an "
            "accidental #include of quark/adapters/mbedtls/*.hpp or quark/adapters/sodium/*.hpp from a CORE header, or a "
            "link-order mistake pulling a security adapter into the default `quark` target.")
    endif()
endforeach()

message(STATUS "security_symbol_isolation: OK -- 0 symbols with prefixes [${SYMBOL_PREFIXES}] in ${TARGET_LIB}")
