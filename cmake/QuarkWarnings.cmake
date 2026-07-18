# Shared warning + sanitizer flags for Quark targets.
# Applied via target_link_libraries(<t> PRIVATE quark_warnings).

add_library(quark_warnings INTERFACE)

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
