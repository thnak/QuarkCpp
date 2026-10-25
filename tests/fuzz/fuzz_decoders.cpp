// libFuzzer entry point (Clang). Build with -DQUARK_BUILD_FUZZ=ON; the CMake target adds
// -fsanitize=fuzzer,address,undefined. The deterministic sibling `fuzz_decoders_test` covers the
// same targets on every platform/compiler.
#include <cstddef>
#include <cstdint>

#include "fuzz_targets.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    quark::fuzz::run_one(data, size);
    return 0;
}
