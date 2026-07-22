// A portable per-test scratch directory (NOT a *_test.cpp, so it is never compiled as a test of its
// own — just an include). POSIX `mkdtemp` has no Windows equivalent; std::filesystem has none either
// (unique_path() was dropped from the original Boost.Filesystem-derived proposal), so this builds its
// own unique name from a clock reading + this thread's id — no OS-specific call needed at all.
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace quark::test {

// Creates (and returns the path of) a fresh directory under the OS temp dir, named
// "<prefix><unique>". Callers own cleanup (std::filesystem::remove_all), matching mkdtemp's contract.
[[nodiscard]] inline std::string make_temp_dir(const std::string& prefix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
        (prefix + std::to_string(now) + "_" + std::to_string(tid));
    std::filesystem::create_directories(dir);
    return dir.string();
}

}  // namespace quark::test
