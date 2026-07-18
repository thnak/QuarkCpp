// Tests 016-Serialization round-trip identity (ADR-016 §"Round-trip"): the tagless POD path AND
// the tagged non-trivial {u64, string, vector<Line>} path both reproduce the original over
// randomized full-range values (incl. NaN / -0.0 doubles) — closing the constant-value dodge.
#include <bit>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "quark/core/serialize.hpp"

using namespace quark;

namespace {

struct Pod {
    std::uint64_t id;
    std::uint32_t qty;
    std::uint32_t flags;
    double price;
};

struct Line {
    std::uint64_t sku;
    std::uint32_t qty;
    double weight;
};

struct Order {
    std::uint64_t id;
    std::string customer;
    std::vector<Line> lines;
};

bool bits_eq(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// QUARK_SERIALIZE must be invoked in the SAME namespace as the type so the generated
// quark_describe is found by ADL (a type's associated namespace is its enclosing one).
QUARK_SERIALIZE(Pod, (1, id), (2, qty), (3, flags), (4, price))
QUARK_SERIALIZE(Line, (1, sku), (2, qty), (3, weight))
QUARK_SERIALIZE(Order, (1, id), (2, customer), (3, lines))

}  // namespace

int main() {
    std::mt19937_64 rng(0xC0FFEE);
    std::uint64_t fails = 0;

    // --- Tagless POD round-trip, 2e5 randomized values (bit-exact, incl. NaN / -0.0) ---------
    constexpr std::uint64_t kPodIters = 200'000;
    for (std::uint64_t i = 0; i < kPodIters; ++i) {
        Pod p{rng(), static_cast<std::uint32_t>(rng()), static_cast<std::uint32_t>(rng()),
              std::bit_cast<double>(rng())};  // random bits -> covers NaN / inf / -0.0
        std::byte buf[64];
        const std::size_t n = encode_tagless(p, buf);
        Pod q{};
        decode_tagless(buf, q);
        if (n != tagless_size(p) || q.id != p.id || q.qty != p.qty || q.flags != p.flags ||
            !bits_eq(q.price, p.price)) {
            if (fails < 5) std::fprintf(stderr, "tagless POD mismatch at %llu\n",
                                        static_cast<unsigned long long>(i));
            ++fails;
        }
    }

    // Also round-trip the POD through the canonical tagged form.
    for (std::uint64_t i = 0; i < 50'000; ++i) {
        Pod p{rng(), static_cast<std::uint32_t>(rng()), static_cast<std::uint32_t>(rng()),
              std::bit_cast<double>(rng())};
        std::byte buf[64];
        auto n = encode_tagged(p, buf, sizeof buf);
        Pod q{};
        auto rc = decode_tagged(buf, *n, q);
        if (!n || !rc || q.id != p.id || q.qty != p.qty || q.flags != p.flags ||
            !bits_eq(q.price, p.price)) {
            ++fails;
        }
    }

    // --- Tagged non-trivial round-trip, 2e4 randomized Orders --------------------------------
    constexpr std::uint64_t kOrderIters = 20'000;
    std::vector<std::byte> buf(4096);
    for (std::uint64_t i = 0; i < kOrderIters; ++i) {
        Order o{};
        o.id = rng();
        const std::size_t clen = rng() % 40;
        for (std::size_t c = 0; c < clen; ++c) o.customer.push_back(static_cast<char>('a' + rng() % 26));
        const std::size_t nlines = rng() % 8;
        for (std::size_t l = 0; l < nlines; ++l) {
            o.lines.push_back(Line{rng(), static_cast<std::uint32_t>(rng()),
                                   std::bit_cast<double>(rng())});
        }
        auto n = encode_tagged(o, buf.data(), buf.size());
        if (!n) { ++fails; continue; }
        Order o2{};
        auto rc = decode_tagged(buf.data(), *n, o2);
        bool ok = rc && o2.id == o.id && o2.customer == o.customer &&
                  o2.lines.size() == o.lines.size();
        for (std::size_t l = 0; ok && l < o.lines.size(); ++l) {
            ok = o2.lines[l].sku == o.lines[l].sku && o2.lines[l].qty == o.lines[l].qty &&
                 bits_eq(o2.lines[l].weight, o.lines[l].weight);
        }
        if (!ok) ++fails;
    }

    const bool pass = (fails == 0);
    std::printf("serialize_roundtrip_test: %s  (pod_fp=%016llx order_fp=%016llx fails=%llu)\n",
                pass ? "OK" : "FAIL", static_cast<unsigned long long>(fingerprint_v<Pod>),
                static_cast<unsigned long long>(fingerprint_v<Order>),
                static_cast<unsigned long long>(fails));
    return pass ? 0 : 1;
}
