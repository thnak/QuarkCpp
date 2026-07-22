# Shared warning + sanitizer flags for Quark targets.
# Applied via target_link_libraries(<t> PRIVATE quark_warnings).

add_library(quark_warnings INTERFACE)

if(MSVC)
  # /std:c++23 is not yet accepted by every MSVC toolset Quark is built with (CI + win-thanh bring-up
  # both landed on /std:c++latest — the verified toolset only recognizes c++14/17/20/latest). /W4 is
  # MSVC's closest match to -Wall -Wextra; /permissive- turns off non-conforming-extension leniency;
  # /Zc:preprocessor opts into the conforming preprocessor (needed by some C++23 macro/attribute use).
  target_compile_options(quark_warnings INTERFACE /W4 /permissive- /Zc:preprocessor /std:c++latest)

  # QUARK_SANITIZE=address (MSVC only ships ASan; no TSan/UBSan — those stay Linux/arm64-only).
  if(QUARK_SANITIZE MATCHES "address")
    if(QUARK_SANITIZE MATCHES "thread" OR QUARK_SANITIZE MATCHES "undefined")
      message(WARNING "Quark: MSVC only supports QUARK_SANITIZE=address; thread/undefined are ignored here.")
    endif()
    message(STATUS "Quark sanitizers (MSVC): address")
    target_compile_options(quark_warnings INTERFACE /fsanitize=address)
  elseif(QUARK_SANITIZE)
    message(WARNING "Quark: QUARK_SANITIZE='${QUARK_SANITIZE}' has no MSVC equivalent (only 'address' is supported) — ignored.")
  endif()
else()
  target_compile_options(quark_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Wunused -Woverloaded-virtual -Wdouble-promotion)

  # QUARK_SANITIZE=address|thread|undefined|"address;undefined"
  if(QUARK_SANITIZE)
    string(REPLACE ";" "," _quark_san "${QUARK_SANITIZE}")
    message(STATUS "Quark sanitizers: ${_quark_san}")
    target_compile_options(quark_warnings INTERFACE
      -fsanitize=${_quark_san} -fno-omit-frame-pointer -g)
    target_link_options(quark_warnings INTERFACE -fsanitize=${_quark_san})
  endif()
endif()
