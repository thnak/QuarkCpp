// Materialize the deterministic seed corpus as raw files for the libFuzzer target.
//   fuzz_seed_corpus <dir>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "fuzz_targets.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path dir(argv[1]);
    std::filesystem::create_directories(dir);
    const auto seeds = quark::fuzz::seed_corpus();
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        std::ofstream out(dir / ("seed_" + std::to_string(i)), std::ios::binary);
        out.write(reinterpret_cast<const char*>(seeds[i].data()),
                  static_cast<std::streamsize>(seeds[i].size()));
    }
    std::printf("fuzz_seed_corpus: wrote %zu seeds to %s\n", seeds.size(), dir.string().c_str());
    return 0;
}
