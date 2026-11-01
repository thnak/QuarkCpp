// Regression for the finding of issue #50 (fuzz of the discovery label decode): decode_instance_label accepted a label
// with a whole spare group of zero bits ("a", "aaa", "...aa") as a second spelling of the same id (and "a" as the EMPTY
// id), so one device could be reported under several instance labels. The decoder is now the strict inverse of the
// encoder: decode(label) succeeds iff label == encode(decode(label)).
//   C1  the aliases that used to decode are refused
//   C2  every id of 1..36 bytes round-trips, and appending a group never yields an alias: the result is refused, or it
//       is a longer id whose own canonical encoding is exactly that label
//   C3  exhaustive over all labels of 1..3 characters: every accepted label is canonical
#include <cstdio>
#include <string>

#include "quark/net/lan_discovery.hpp"

using namespace quark;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}
}  // namespace

int main() {
    // C1
    check(!net::decode_instance_label("a"), "C1: 'a' (five zero bits, no byte) is not an id");
    check(!net::decode_instance_label("aaa"), "C1: 'aaa' is not a second spelling of {0}");
    check(net::decode_instance_label("aa") == net::DnsSdBytes{std::byte{0}}, "C1 control: 'aa' is {0}");

    // C2
    for (std::size_t n = 1; n <= net::kMaxInstanceIdBytes; ++n) {
        net::DnsSdBytes id(n);
        for (std::size_t i = 0; i < n; ++i) id[i] = static_cast<std::byte>((n * 31 + i * 17) & 0xFF);
        const std::string enc = net::encode_instance_label(id);
        check(net::decode_instance_label(enc) == id, "C2: round trip");
        const auto longer = net::decode_instance_label(enc + "a");
        check(!longer || net::encode_instance_label(*longer) == enc + "a", "C2: a spare 'a' group is no alias");
    }

    // C3
    static constexpr char kAlpha[] = "abcdefghijklmnopqrstuvwxyz234567";
    for (int len = 1; len <= 3; ++len) {
        std::string s(static_cast<std::size_t>(len), 'a');
        const long total = len == 1 ? 32 : len == 2 ? 1024 : 32768;
        for (long v = 0; v < total; ++v) {
            long x = v;
            for (int i = len - 1; i >= 0; --i) {
                s[static_cast<std::size_t>(i)] = kAlpha[x % 32];
                x /= 32;
            }
            if (const auto d = net::decode_instance_label(s))
                check(net::encode_instance_label(*d) == s, "C3: an accepted label is canonical");
        }
    }

    std::puts(g_ok ? "lan_discovery_label_canonical_test: OK" : "lan_discovery_label_canonical_test: FAILED");
    return g_ok ? 0 : 1;
}
