# Quark sample projects — small, runnable programs that show how to AUTHOR actors and drive the
# engine through the 005 (developer model) / 006 (messaging) surface. Each subdirectory that holds a
# `main.cpp` becomes one executable named after the directory. Auto-discovered: add a dir with a
# main.cpp and reconfigure.
#
# These are NOT part of the engine and NOT a correctness gate (the tests/ suite is that) — they exist
# to be read and run. Build them with:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build
# Run machine-safe, pinned to <=4 cores:  taskset -c 0-3 build/samples/01_hello_counter

find_package(Threads REQUIRED)

file(GLOB _quark_sample_dirs RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(dir ${_quark_sample_dirs})
  set(_main "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/main.cpp")
  if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${dir}" AND EXISTS "${_main}")
    add_executable(${dir} "${_main}")
    target_link_libraries(${dir} PRIVATE quark::quark quark_warnings Threads::Threads)
  endif()
endforeach()
