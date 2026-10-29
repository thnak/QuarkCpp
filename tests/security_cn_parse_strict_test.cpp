// Tests ADR-067 D3 — the identity CN grammar (`parse_quark_cn`, tls_identity.hpp) and the NodeId client
// bit (`kClientNodeBit`, ids.hpp). Std-only: the mbedTLS adapter's single-CN and CA-leaf checks are in
// tests/adapters/mbedtls_handshake_check.cpp.
//
// Load-bearing properties:
//   * one identity has one spelling: both numeric fields are canonical decimal, consumed exactly;
//   * the role is the prefix AND the id bit, and they must agree — a client can never present a member
//     id (red-team round 1 finding #1) and a member can never present a client id;
//   * a `quark-client:` CN is refused by a parse that only knows `quark:` (old nodes fail closed).
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "quark/core/ids.hpp"
#include "quark/core/tls_identity.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

bool parses(std::string_view cn) {
    ClusterId c{};
    NodeId n{};
    return parse_quark_cn(cn, c, n);
}

// The pre-ADR-067 member-only grammar (prefix compare, then "<cluster>:<node>"), reproduced to show an
// un-upgraded node's prefix check cannot accept a client CN, whatever follows the prefix.
bool legacy_prefix_accepts(std::string_view cn) {
    constexpr std::string_view kPrefix = "quark:";
    return cn.size() > kPrefix.size() && cn.substr(0, kPrefix.size()) == kPrefix;
}
}  // namespace

int main() {
    bool ok = true;
    const std::uint64_t client_id = kClientNodeBit | 5;
    const std::string client_cn = "quark-client:7:" + std::to_string(client_id);

    // ---- positive controls -------------------------------------------------------------------
    {
        ClusterId c{};
        NodeId n{};
        check(parse_quark_cn("quark:7:5", c, n), "member CN parses", ok);
        check(c == ClusterId{7} && n == NodeId{5}, "member CN yields cluster 7, node 5", ok);
        check(!is_client_node(n), "member id has the client bit clear", ok);

        check(parse_quark_cn(client_cn, c, n), "client CN with the client bit parses", ok);
        check(c == ClusterId{7} && n == NodeId{client_id}, "client CN yields cluster 7, client id", ok);
        check(is_client_node(n), "client id has the client bit set", ok);

        check(parses("quark:0:0"), "zero is canonical", ok);
        check(parses("quark:18446744073709551615:9223372036854775807"),
              "max cluster and max member id parse", ok);
    }

    // ---- outputs untouched on refusal ----------------------------------------------------------
    {
        ClusterId c{11};
        NodeId n{12};
        check(!parse_quark_cn("quark:7:5x", c, n), "trailing byte refused", ok);
        check(c == ClusterId{11} && n == NodeId{12}, "refusal leaves outputs unchanged", ok);
    }

    // ---- strict numeric fields (red-team round 1 finding #12) ----------------------------------
    check(!parses("quark:7:5x"), "trailing garbage after node refused", ok);
    check(!parses("quark:7:5:c"), "suffix after node refused (the rejected suffix encoding)", ok);
    check(!parses("quark:7x:5"), "trailing garbage after cluster refused", ok);
    check(!parses("quark:7:5 "), "trailing space refused", ok);
    check(!parses("quark: 7:5"), "leading space refused", ok);
    check(!parses("quark:07:5"), "leading zero in cluster refused", ok);
    check(!parses("quark:7:05"), "leading zero in node refused", ok);
    check(!parses("quark:+7:5"), "plus sign refused", ok);
    check(!parses("quark:-7:5"), "minus sign refused", ok);
    check(!parses("quark:7:-5"), "minus sign in node refused", ok);
    check(!parses("quark:18446744073709551616:5"), "cluster overflow refused", ok);
    check(!parses("quark:7:18446744073709551616"), "node overflow refused", ok);
    check(!parses("quark::5"), "empty cluster refused", ok);
    check(!parses("quark:7:"), "empty node refused", ok);
    check(!parses("quark:7"), "missing node field refused", ok);
    check(!parses("quark:"), "bare prefix refused", ok);
    check(!parses(""), "empty CN refused", ok);
    check(!parses(std::string_view("quark:7:5\0", 10)), "embedded NUL refused", ok);

    // ---- prefix ------------------------------------------------------------------------------
    check(!parses("Quark:7:5"), "prefix is case-sensitive", ok);
    check(!parses("quarkclient:7:5"), "near-miss prefix refused", ok);
    check(!parses("quark-client-x:7:" + std::to_string(client_id)), "unknown prefix refused", ok);
    check(!parses("xquark:7:5"), "prefix must be at the start", ok);

    // ---- role and id bit must agree (red-team round 1 finding #1) -------------------------------
    check(!parses("quark-client:7:5"), "client CN carrying a MEMBER id refused", ok);
    check(!parses("quark:7:" + std::to_string(client_id)), "member CN carrying a CLIENT id refused", ok);
    check(!parses("quark:7:9223372036854775808"), "member CN with exactly the client bit refused", ok);

    // ---- old nodes fail closed -----------------------------------------------------------------
    check(!legacy_prefix_accepts(client_cn), "a member-only prefix check refuses a client CN", ok);
    check(legacy_prefix_accepts("quark:7:5"), "control: the member-only check accepts a member CN", ok);

    std::printf("security_cn_parse_strict_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
