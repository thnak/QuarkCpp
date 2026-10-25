// P5 deterministic decoder fuzz (every platform/compiler; runs in the default ctest).
//
// Seeds every target from `fuzz/fuzz_targets.hpp`, then drives a deterministic random/mutated
// campaign over: 016 tagged + bounds-checked tagless, the F17 mixed-schema pair + gate, SWIM
// control, schema advert, durable records + migration, frame bodies, TCP frame reassembly, and a
// random bytes-in-.qwal FileStore replay. Any ASan/UBSan trap or crash is a finding.
//
//   fuzz_decoders_test [--iters N]   (default 50000; CI/lab use 2000000 for the long campaign)
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "fuzz/fuzz_targets.hpp"
#include "quark/core/file_store.hpp"

using namespace quark;

namespace {
std::uint64_t parse_iters(int argc, char** argv) {
    std::uint64_t iters = 50000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = std::strtoull(argv[++i], nullptr, 0);
        } else {
            iters = std::strtoull(argv[i], nullptr, 0);
        }
    }
    return iters;
}

std::filesystem::path unique_dir() {
    const auto n = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("quark_fuzz_wal_" + std::to_string(static_cast<long long>(n)));
}
}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t iters = parse_iters(argc, argv);
    const auto seeds = fuzz::seed_corpus();
    for (const auto& s : seeds) fuzz::run_one(s.data(), s.size());

    std::mt19937_64 rng{0x5EEDF00D1234ULL};
    std::vector<std::uint8_t> buf;
    for (std::uint64_t i = 0; i < iters; ++i) {
        const std::size_t len = 1 + static_cast<std::size_t>(rng() % 2048);
        buf.resize(len);
        for (auto& b : buf) b = static_cast<std::uint8_t>(rng() & 0xFF);
        fuzz::run_one(buf.data(), buf.size());
        if ((i + 1) % 250000 == 0) {
            std::printf("fuzz_decoders_test: %llu/%llu\n",
                        static_cast<unsigned long long>(i + 1),
                        static_cast<unsigned long long>(iters));
            std::fflush(stdout);
        }
    }

    // Persistence frame parser: feed random bytes as a `.qwal`, then reopen. The replay must either
    // truncate at a torn/CRC-bad record or apply a well-formed one — never read out of bounds.
    const std::filesystem::path dir = unique_dir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const ActorId who{TypeKey{0xABCDEF}, 0x1234};
    char name[64];
    std::snprintf(name, sizeof name, "%016llx-%016llx.qwal",
                  static_cast<unsigned long long>(who.type.value),
                  static_cast<unsigned long long>(who.key));
    const std::filesystem::path wal = dir / name;
    for (int i = 0; i < 100; ++i) {
        const std::size_t len = static_cast<std::size_t>(rng() % 4096);
        std::vector<std::uint8_t> bytes(len);
        for (auto& b : bytes) b = static_cast<std::uint8_t>(rng() & 0xFF);
        {
            std::ofstream out(wal, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        FileStore store(dir.string());
        (void)store.read_log(who, SeqNo{0});
        (void)store.load_snapshot(who);
    }
    std::filesystem::remove_all(dir);

    std::printf("fuzz_decoders_test: OK (seeds=%zu iters=%llu + 100 random WAL replays)\n",
                seeds.size(), static_cast<unsigned long long>(iters));
    return 0;
}
