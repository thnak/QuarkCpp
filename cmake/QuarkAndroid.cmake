# Implements 019-Platform-Abstraction-Layer §"Android target" (Draft amendment); ADR-071 (Android PAL +
# mobile lifecycle). Included from the top-level CMakeLists.txt ONLY when CMAKE_SYSTEM_NAME is Android,
# i.e. when configured through the NDK's `android.toolchain.cmake`:
#
#   cmake -S . -B build-android-arm64 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 -DANDROID_STL=c++_static
#
# What this module decides (and why):
#   * ABIs: arm64-v8a (first-class) and x86_64 (emulator / test). 32-bit ABIs are refused.
#   * Minimum API level 35 (Android 15, AeroCoWorker's client floor); a lower level is refused, not
#     silently accepted (the PAL's epoll/getrandom/CLOCK_BOOTTIME assumptions are verified at 35).
#   * Event loop: epoll only. io_uring is NOT selectable here: bionic's seccomp allowlist for
#     zygote-spawned processes is SYSCALLS.TXT minus the blocklist, and io_uring_* is absent from
#     SYSCALLS.TXT (ADR-071 §3). The Linux PAL backend (pal/linux_x86_64/) already compiles to epoll.
#   * libc++ 19 (NDK r28) ships std::stop_token/stop_source/jthread as an *experimental* library; the
#     core uses them (001 cooperative cancellation), so -fexperimental-library is required both to
#     compile and to link libc++experimental.a. It is a global flag so try_compile() probes inherit it.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
  return()
endif()

if(NOT ANDROID_ABI MATCHES "^(arm64-v8a|x86_64)$")
  message(FATAL_ERROR "Quark/Android: ANDROID_ABI='${ANDROID_ABI}' is unsupported; use arm64-v8a "
                      "(first-class) or x86_64 (emulator/test) — ADR-071 §2.")
endif()
if(ANDROID_PLATFORM_LEVEL LESS 35)
  message(FATAL_ERROR "Quark/Android: API level ${ANDROID_PLATFORM_LEVEL} is below the supported floor 35 "
                      "(Android 15) — pass -DANDROID_PLATFORM=android-35 (ADR-071 §2).")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
  message(FATAL_ERROR "Quark/Android: NDK clang ${CMAKE_CXX_COMPILER_VERSION} is too old (need NDK r28+, clang 19).")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexperimental-library")
set(QUARK_ANDROID TRUE)

# The shared library the Android app loads through JNI (no JNI symbols here: the app's own glue
# library links this one). Defined by the top-level CMakeLists after the `quark` target exists.
function(quark_add_android_targets)
  add_library(quark_android SHARED src/android/quark_android.cpp)
  target_link_libraries(quark_android PRIVATE quark::quark quark_warnings)
  set_target_properties(quark_android PROPERTIES
      CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  # Honest size: -Wl,--gc-sections and strip (see ADR-071 §7 for the measured numbers).
  target_link_options(quark_android PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384)
endfunction()
