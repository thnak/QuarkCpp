// Issue #51 (assessment M6): the C++ half of the differential test of the Noise IK glue. A TOOL, not a library dependency: it reads
// transcript cases from stdin (one per line, space-separated hex, "-" = empty), drives quark::adapters::noise::IkHandshake in BOTH
// roles with the case's fixed keys, ephemerals and payloads, and prints per case, as hex: msg1 msg2 hash_initiator hash_responder
// split keys (initiator send, initiator recv, responder send, responder recv) and the first transport ciphertext in each direction.
// tests/adapters/noise_ik_differential.py generates the cases, runs the independent reference (the Python `noiseprotocol` package) over
// the same inputs and compares every field byte for byte.
//   input line:  prologue init_static init_ephemeral resp_static resp_ephemeral payload1 payload2 transport1 transport2
// The same source is also built against copies of noise.hpp with ONE line changed in MixHash / MixKey (CMake, see
// QuarkSecurityAdapters.cmake): those control tools must be reported as DIFFERENT by the driver.
// Implements ADR-068 claim V1 (stronger evidence than the single official vector); 020-Security §"Pinned-key mode (Noise IK, #40)".
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "quark/adapters/sodium/noise.hpp"

using namespace quark::adapters::noise;

namespace {
Bytes from_hex(const std::string& h) {
    Bytes b;
    if (h == "-") return b;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2)
        b.push_back(static_cast<std::byte>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return b;
}
template <class A>
std::string hex(const A& a) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (const std::byte x : a) {
        s.push_back(d[static_cast<unsigned>(x) >> 4]);
        s.push_back(d[static_cast<unsigned>(x) & 15]);
    }
    return s.empty() ? "-" : s;
}
Key32 key_from_hex(const std::string& h) {
    Key32 k{};
    const Bytes b = from_hex(h);
    std::copy(b.begin(), b.end(), k.begin());
    return k;
}
KeyPair pair_from(const Key32& sk) {
    KeyPair kp;
    kp.sk = sk;
    kp.pk = public_from_private(sk);
    return kp;
}
}  // namespace

int main() {
    if (!init()) {
        std::fprintf(stderr, "libsodium unavailable\n");
        return 1;
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream is(line);
        std::string prol, si_h, ei_h, sr_h, er_h, p1_h, p2_h, t1_h, t2_h;
        if (!(is >> prol >> si_h >> ei_h >> sr_h >> er_h >> p1_h >> p2_h >> t1_h >> t2_h)) {
            std::fprintf(stderr, "bad input line\n");
            return 2;
        }
        const Bytes prologue = from_hex(prol);
        const KeyPair si = pair_from(key_from_hex(si_h)), sr = pair_from(key_from_hex(sr_h));
        const Key32 ei = key_from_hex(ei_h), er = key_from_hex(er_h);
        const Key32 rs = sr.pk;  // the initiator pins the responder's static public key

        IkHandshake ini(Role::Initiator, prologue, si, &rs, [&](Key32& sk) { sk = ei; });
        IkHandshake rsp(Role::Responder, prologue, sr, nullptr, [&](Key32& sk) { sk = er; });
        Bytes msg1, msg2, got1, got2;
        if (!ini.write_msg1(from_hex(p1_h), msg1) || !rsp.read_msg1(msg1, got1) || !rsp.write_msg2(from_hex(p2_h), msg2) ||
            !ini.read_msg2(msg2, got2)) {
            std::printf("FAIL\n");
            continue;
        }
        CipherState i_send, i_recv, r_send, r_recv;
        if (!ini.split(i_send, i_recv) || !rsp.split(r_send, r_recv)) {
            std::printf("FAIL\n");
            continue;
        }
        Bytes c1, c2;
        if (!i_send.encrypt_with_ad({}, from_hex(t1_h), c1) || !r_send.encrypt_with_ad({}, from_hex(t2_h), c2)) {
            std::printf("FAIL\n");
            continue;
        }
        std::printf("%s %s %s %s %s %s %s %s %s %s\n", hex(msg1).c_str(), hex(msg2).c_str(), hex(ini.handshake_hash()).c_str(),
                    hex(rsp.handshake_hash()).c_str(), hex(i_send.key()).c_str(), hex(i_recv.key()).c_str(),
                    hex(r_send.key()).c_str(), hex(r_recv.key()).c_str(), hex(c1).c_str(), hex(c2).c_str());
    }
    return 0;
}
