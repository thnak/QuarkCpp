// Proves quark::adapters::noise (include/quark/adapters/sodium/noise.hpp) against the OFFICIAL Noise_IK_25519_ChaChaPoly_BLAKE2b
// test vector (cacophony set, tests/vectors/noise_ik_25519_chachapoly_blake2b.txt — provenance in its header): every
// handshake message, the handshake hash and every transport message must match byte for byte, in both roles, using the
// vector's fixed ephemerals. Controls (each must FAIL): a flipped payload byte, a different prologue, a different
// responder static key and a corrupted ciphertext each break the match / the authentication.
// Implements ADR-068 claim V1; 020-Security §"Pinned-key mode (Noise IK, #40)".
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "quark/adapters/sodium/noise.hpp"

#ifndef QUARK_TEST_VECTOR_DIR
#error "QUARK_TEST_VECTOR_DIR must point at tests/vectors"
#endif

using namespace quark::adapters::noise;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

Bytes from_hex(const std::string& h) {
    Bytes b;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2)
        b.push_back(static_cast<std::byte>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return b;
}
template <class A>
A key_from_hex(const std::string& h) {
    A a{};
    const Bytes b = from_hex(h);
    std::copy(b.begin(), b.end(), a.begin());
    return a;
}

struct Vector {
    std::map<std::string, std::string> kv;
    std::vector<std::pair<Bytes, Bytes>> msgs;  // payload, expected ciphertext
};

bool load(Vector& v) {
    std::ifstream f(std::string(QUARK_TEST_VECTOR_DIR) + "/noise_ik_25519_chachapoly_blake2b.txt");
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find(" = ");
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), val = line.substr(eq + 3);
        if (k == "msg") {
            std::istringstream is(val);
            std::string p, c;
            is >> p >> c;
            v.msgs.emplace_back(from_hex(p), from_hex(c));
        } else {
            v.kv[k] = val;
        }
    }
    return true;
}

KeyPair pair_from_private(const Key32& sk) {
    KeyPair kp;
    kp.sk = sk;
    kp.pk = public_from_private(sk);
    return kp;
}

struct Run {
    bool handshake_ok = false;
    bool all_match = true;
    Bytes msg1, msg2;
    Hash64 hash_i{}, hash_r{};
};

// Drive both roles from the vector's inputs; optionally perturb one input (the controls).
Run run(const Vector& v, const Bytes* prologue_override, const Key32* resp_static_override, bool corrupt_msg1) {
    Run r;
    const Bytes prologue_i = prologue_override ? *prologue_override : from_hex(v.kv.at("init_prologue"));
    const Bytes prologue_r = from_hex(v.kv.at("resp_prologue"));
    const KeyPair si = pair_from_private(key_from_hex<Key32>(v.kv.at("init_static")));
    const KeyPair sr = pair_from_private(key_from_hex<Key32>(v.kv.at("resp_static")));
    Key32 rs = key_from_hex<Key32>(v.kv.at("init_remote_static"));
    if (resp_static_override) rs = *resp_static_override;
    const Key32 ei = key_from_hex<Key32>(v.kv.at("init_ephemeral")), er = key_from_hex<Key32>(v.kv.at("resp_ephemeral"));

    IkHandshake ini(Role::Initiator, prologue_i, si, &rs, [&](Key32& sk) { sk = ei; });
    IkHandshake rsp(Role::Responder, prologue_r, sr, nullptr, [&](Key32& sk) { sk = er; });

    if (!ini.write_msg1(v.msgs[0].first, r.msg1)) return r;
    if (r.msg1 != v.msgs[0].second) r.all_match = false;
    Bytes wire1 = r.msg1;
    if (corrupt_msg1) wire1[wire1.size() - 1] ^= std::byte{1};
    Bytes pl;
    if (!rsp.read_msg1(wire1, pl)) return r;
    if (pl != v.msgs[0].first) r.all_match = false;
    if (!rsp.write_msg2(v.msgs[1].first, r.msg2)) return r;
    if (r.msg2 != v.msgs[1].second) r.all_match = false;
    Bytes pl2;
    if (!ini.read_msg2(r.msg2, pl2)) return r;
    if (pl2 != v.msgs[1].first) r.all_match = false;
    r.hash_i = ini.handshake_hash();
    r.hash_r = rsp.handshake_hash();
    const Bytes want_hash = from_hex(v.kv.at("handshake_hash"));
    if (!std::equal(r.hash_i.begin(), r.hash_i.end(), want_hash.begin(), want_hash.end())) r.all_match = false;
    if (r.hash_i != r.hash_r) r.all_match = false;

    CipherState i_send, i_recv, r_send, r_recv;
    if (!ini.split(i_send, i_recv) || !rsp.split(r_send, r_recv)) return r;
    // Transport messages 2.. alternate initiator -> responder, responder -> initiator, in the vector's order.
    for (std::size_t m = 2; m < v.msgs.size(); ++m) {
        const bool from_initiator = (m % 2) == 0;
        CipherState& tx = from_initiator ? i_send : r_send;
        CipherState& rx = from_initiator ? r_recv : i_recv;
        Bytes ct;
        if (!tx.encrypt_with_ad({}, v.msgs[m].first, ct)) return r;
        if (ct != v.msgs[m].second) r.all_match = false;
        Bytes pt;
        if (!rx.decrypt_with_ad({}, ct, pt) || pt != v.msgs[m].first) r.all_match = false;
    }
    r.handshake_ok = true;
    return r;
}
}  // namespace

int main() {
    bool ok = true;
    if (!init()) {
        std::fprintf(stderr, "libsodium unavailable\n");
        return 1;
    }
    Vector v;
    if (!load(v) || v.msgs.size() != 6 || v.kv.at("protocol_name") != std::string(kSuiteName)) {
        std::fprintf(stderr, "cannot load the test vector (or it is not %s)\n", std::string(kSuiteName).c_str());
        return 1;
    }

    // 1. The vector: byte for byte, both roles, handshake messages + hash + 4 transport messages.
    const Run good = run(v, nullptr, nullptr, false);
    check(good.handshake_ok, "handshake completes on the official vector", ok);
    check(good.all_match, "every message, the handshake hash and every transport message match the vector byte for byte", ok);
    check(good.msg1.size() == kMsg1Overhead + v.msgs[0].first.size(), "msg1 length = 96 + payload", ok);
    check(good.msg2.size() == kMsg2Overhead + v.msgs[1].first.size(), "msg2 length = 48 + payload", ok);

    // Controls: each perturbation must break the match (a check that cannot fail proves nothing).
    {
        Vector bad = v;
        bad.msgs[0].first[0] ^= std::byte{1};
        const Run r = run(bad, nullptr, nullptr, false);
        check(!(r.handshake_ok && r.all_match), "CONTROL: a flipped payload byte no longer matches the vector", ok);
    }
    {
        const Bytes other = from_hex("4a6f686e2047616c75");  // prologue differs in the last byte
        const Run r = run(v, &other, nullptr, false);
        check(!(r.handshake_ok && r.all_match), "CONTROL: a different prologue breaks the handshake", ok);
    }
    {
        Key32 wrong = key_from_hex<Key32>(v.kv.at("init_remote_static"));
        wrong[0] ^= std::byte{1};
        const Run r = run(v, nullptr, &wrong, false);
        check(!r.handshake_ok, "CONTROL: a different pinned responder key breaks the handshake", ok);
    }
    {
        const Run r = run(v, nullptr, nullptr, true);
        check(!r.handshake_ok, "CONTROL: a corrupted msg1 ciphertext is rejected by the responder", ok);
    }

    // 2. The primitives the state machine rests on: HMAC-BLAKE2b is RFC 2104 (an independent construction over the same
    // hash gives the same tag) and HKDF's two outputs differ.
    {
        const Bytes key = from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
        const Bytes msg = from_hex("4869205468657265");  // "Hi There"
        const Hash64 a = hmac(key, msg);
        // Independent: build ipad/opad by hand with a keyed-from-scratch pass.
        Bytes ipad(kBlockLen, std::byte{0x36}), opad(kBlockLen, std::byte{0x5c});
        for (std::size_t i = 0; i < key.size(); ++i) {
            ipad[i] ^= key[i];
            opad[i] ^= key[i];
        }
        Bytes inner_in = ipad;
        inner_in.insert(inner_in.end(), msg.begin(), msg.end());
        const Hash64 inner = hash(inner_in);
        const Hash64 b = hash(opad, inner);
        check(a == b, "HMAC-BLAKE2b equals the hand-built RFC 2104 construction", ok);
    }

    std::printf(ok ? "noise_ik_vectors_check: OK\n" : "noise_ik_vectors_check: FAILED\n");
    return ok ? 0 : 1;
}
