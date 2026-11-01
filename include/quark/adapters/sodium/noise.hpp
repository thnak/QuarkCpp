// Implements 020-Security §"Pinned-key mode (Noise IK, #40)" and ADR-068 — the Noise Protocol Framework
// (revision 34, noiseprotocol.org) written ONLY from libsodium primitives, for the one suite Quark uses:
//
//     Noise_IK_25519_ChaChaPoly_BLAKE2b
//
// Primitives, all libsodium and nothing else:
//   DH      X25519                   crypto_scalarmult / crypto_scalarmult_base   (all-zero output rejected)
//   Cipher  ChaCha20-Poly1305 (IETF) crypto_aead_chacha20poly1305_ietf_*          (nonce = 4 zero bytes || LE64 n)
//   Hash    BLAKE2b-512              crypto_generichash                            (HASHLEN 64)
//   HMAC    HMAC-BLAKE2b             built here from crypto_generichash per RFC 2104 (BLOCKLEN 128), as Noise §4.3
//   HKDF    Noise's HKDF             built here from the HMAC above, as Noise §4.3
//
// THE HONEST EXCEPTION (020 §2): the cryptographic PRIMITIVES are libsodium's. What is written here is the
// Noise STATE MACHINE (SymmetricState / CipherState / the IK message pattern), which is a few dozen lines of
// glue over those primitives. It is proven against the official cacophony test vector for exactly this suite
// (tests/adapters/noise_ik_vectors_check.cpp, tests/vectors/), byte for byte, and it is not a substitute for an
// independent audit (ADR-068, "What remains unproven").
//
// Not in this file: the Quark protocol on top (payload layout, trust provider, replay rule) — see
// pinned_handshake.hpp. This file is Noise only, so it can be tested against the vectors in isolation.
//
// Opt-in adapter: compiled only with QUARK_WITH_SODIUM=ON (cmake/QuarkSecurityAdapters.cmake); the core never
// includes it.
#pragma once

#include <sodium.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace quark::adapters::noise {

inline constexpr std::size_t kDhLen = 32;     // X25519 public/private key and DH output
inline constexpr std::size_t kHashLen = 64;   // BLAKE2b-512
inline constexpr std::size_t kBlockLen = 128; // BLAKE2b block size (HMAC)
inline constexpr std::size_t kKeyLen = 32;    // ChaCha20-Poly1305 key
inline constexpr std::size_t kTagLen = 16;    // Poly1305 tag

using Bytes = std::vector<std::byte>;
using Key32 = std::array<std::byte, kKeyLen>;
using Hash64 = std::array<std::byte, kHashLen>;

[[nodiscard]] inline const unsigned char* uc(const std::byte* p) noexcept {
    return reinterpret_cast<const unsigned char*>(p);
}
[[nodiscard]] inline unsigned char* uc(std::byte* p) noexcept { return reinterpret_cast<unsigned char*>(p); }

// sodium_init() is thread-safe and idempotent; false means libsodium cannot run (fail closed everywhere).
[[nodiscard]] inline bool init() noexcept {
    static const int rc = ::sodium_init();
    return rc >= 0;
}

inline void wipe(std::span<std::byte> s) noexcept {
    if (!s.empty()) ::sodium_memzero(s.data(), s.size());
}

// ---- Hash / HMAC / HKDF (Noise §4.3) --------------------------------------------------------------
[[nodiscard]] inline Hash64 hash(std::span<const std::byte> a, std::span<const std::byte> b = {}) noexcept {
    Hash64 out{};
    ::crypto_generichash_state st;
    ::crypto_generichash_init(&st, nullptr, 0, kHashLen);
    if (!a.empty()) ::crypto_generichash_update(&st, uc(a.data()), a.size());
    if (!b.empty()) ::crypto_generichash_update(&st, uc(b.data()), b.size());
    ::crypto_generichash_final(&st, uc(out.data()), kHashLen);
    ::sodium_memzero(&st, sizeof st);
    return out;
}

// HMAC-BLAKE2b (RFC 2104 with H = BLAKE2b-512, B = 128). `key` is at most kBlockLen bytes here (the callers
// pass a 64-byte chaining key / temp key or a 32-byte key); a longer key is hashed first as RFC 2104 says.
[[nodiscard]] inline Hash64 hmac(std::span<const std::byte> key, std::span<const std::byte> a,
                                 std::span<const std::byte> b = {}) noexcept {
    std::array<std::byte, kBlockLen> k0{};
    if (key.size() > kBlockLen) {
        Hash64 hk = hash(key);
        std::memcpy(k0.data(), hk.data(), hk.size());
        wipe(hk);
    } else if (!key.empty()) {
        std::memcpy(k0.data(), key.data(), key.size());
    }
    std::array<std::byte, kBlockLen> ipad{}, opad{};
    for (std::size_t i = 0; i < kBlockLen; ++i) {
        ipad[i] = k0[i] ^ std::byte{0x36};
        opad[i] = k0[i] ^ std::byte{0x5c};
    }
    const Hash64 inner = [&] {
        Hash64 out{};
        ::crypto_generichash_state st;
        ::crypto_generichash_init(&st, nullptr, 0, kHashLen);
        ::crypto_generichash_update(&st, uc(ipad.data()), ipad.size());
        if (!a.empty()) ::crypto_generichash_update(&st, uc(a.data()), a.size());
        if (!b.empty()) ::crypto_generichash_update(&st, uc(b.data()), b.size());
        ::crypto_generichash_final(&st, uc(out.data()), kHashLen);
        ::sodium_memzero(&st, sizeof st);  // its buffered block held k0 ^ ipad
        return out;
    }();
    const Hash64 out = hash(std::span<const std::byte>(opad), std::span<const std::byte>(inner));
    wipe(k0);
    wipe(ipad);
    wipe(opad);
    return out;
}

// Noise HKDF producing 2 outputs (the only arity IK needs besides Split, which is also 2).
inline void hkdf2(std::span<const std::byte> chaining_key, std::span<const std::byte> ikm, Hash64& out1,
                  Hash64& out2) noexcept {
    Hash64 temp = hmac(chaining_key, ikm);
    const std::byte one{0x01}, two{0x02};
    out1 = hmac(temp, std::span<const std::byte>(&one, 1));
    out2 = hmac(temp, std::span<const std::byte>(out1), std::span<const std::byte>(&two, 1));
    wipe(temp);
}

// ---- DH -------------------------------------------------------------------------------------------
[[nodiscard]] inline Key32 public_from_private(const Key32& sk) noexcept {
    Key32 pk{};
    ::crypto_scalarmult_base(uc(pk.data()), uc(sk.data()));
    return pk;
}

// False when libsodium rejects the result (a low-order/invalid public key yields the all-zero output).
[[nodiscard]] inline bool dh(const Key32& sk, const Key32& pk, Key32& out) noexcept {
    return ::crypto_scalarmult(uc(out.data()), uc(sk.data()), uc(pk.data())) == 0;
}

struct KeyPair {
    Key32 sk{};
    Key32 pk{};
};

// ---- CipherState (Noise §5.1) ---------------------------------------------------------------------
class CipherState {
public:
    CipherState() = default;
    CipherState(const CipherState&) = delete;
    CipherState& operator=(const CipherState&) = delete;
    CipherState(CipherState&& o) noexcept : k_(o.k_), n_(o.n_), has_key_(o.has_key_) { o.reset(); }
    CipherState& operator=(CipherState&& o) noexcept {
        if (this != &o) {
            k_ = o.k_;
            n_ = o.n_;
            has_key_ = o.has_key_;
            o.reset();
        }
        return *this;
    }
    ~CipherState() { reset(); }

    void initialize_key(const Key32& k) noexcept {
        k_ = k;
        n_ = 0;
        has_key_ = true;
    }
    [[nodiscard]] bool has_key() const noexcept { return has_key_; }
    // The raw key. Exposed ONLY so the Quark adapter can derive its session envelope keys from Noise's Split()
    // outputs under a Quark-specific label (pinned_handshake.hpp); nothing else should read it.
    [[nodiscard]] const Key32& key() const noexcept { return k_; }
    [[nodiscard]] std::uint64_t nonce() const noexcept { return n_; }
    void set_nonce(std::uint64_t n) noexcept { n_ = n; }

    // EncryptWithAd: without a key this is the identity (Noise §5.1); with one it appends ciphertext||tag
    // to `out`. False when the nonce space is exhausted (n == 2^64-1 is reserved, Noise §5.1).
    [[nodiscard]] bool encrypt_with_ad(std::span<const std::byte> ad, std::span<const std::byte> pt,
                                       Bytes& out) {
        if (!has_key_) {
            out.insert(out.end(), pt.begin(), pt.end());
            return true;
        }
        if (n_ == UINT64_MAX) return false;
        const std::size_t base = out.size();
        out.resize(base + pt.size() + kTagLen);
        unsigned long long clen = 0;
        unsigned char npub[12];
        make_nonce(npub);
        ::crypto_aead_chacha20poly1305_ietf_encrypt(uc(out.data() + base), &clen, pt.empty() ? nullptr : uc(pt.data()),
                                                    pt.size(), ad.empty() ? nullptr : uc(ad.data()), ad.size(),
                                                    nullptr, npub, uc(k_.data()));
        ++n_;
        return clen == pt.size() + kTagLen;
    }

    // DecryptWithAd: false on an authentication failure (the nonce does NOT advance, Noise §5.1).
    [[nodiscard]] bool decrypt_with_ad(std::span<const std::byte> ad, std::span<const std::byte> ct,
                                       Bytes& out) {
        if (!has_key_) {
            out.insert(out.end(), ct.begin(), ct.end());
            return true;
        }
        if (n_ == UINT64_MAX || ct.size() < kTagLen) return false;
        const std::size_t base = out.size();
        out.resize(base + ct.size() - kTagLen);
        unsigned char dummy = 0;
        unsigned long long mlen = 0;
        unsigned char npub[12];
        make_nonce(npub);
        const int rc = ::crypto_aead_chacha20poly1305_ietf_decrypt(
            out.size() == base ? &dummy : uc(out.data() + base), &mlen, nullptr, uc(ct.data()), ct.size(),
            ad.empty() ? nullptr : uc(ad.data()), ad.size(), npub, uc(k_.data()));
        if (rc != 0) {
            out.resize(base);
            return false;
        }
        ++n_;
        return true;
    }

private:
    void make_nonce(unsigned char (&npub)[12]) const noexcept {
        std::memset(npub, 0, 4);
        for (int i = 0; i < 8; ++i) npub[4 + i] = static_cast<unsigned char>((n_ >> (8 * i)) & 0xFF);
    }
    void reset() noexcept {
        wipe(k_);
        n_ = 0;
        has_key_ = false;
    }
    Key32 k_{};
    std::uint64_t n_ = 0;
    bool has_key_ = false;
};

// ---- SymmetricState (Noise §5.2) ------------------------------------------------------------------
class SymmetricState {
public:
    SymmetricState() = default;
    SymmetricState(const SymmetricState&) = delete;
    SymmetricState& operator=(const SymmetricState&) = delete;
    ~SymmetricState() { wipe(ck_); }

    void initialize(std::string_view protocol_name) noexcept {
        const auto name = std::as_bytes(std::span<const char>(protocol_name.data(), protocol_name.size()));
        if (name.size() <= kHashLen) {
            h_.fill(std::byte{0});
            std::memcpy(h_.data(), name.data(), name.size());
        } else {
            h_ = hash(name);
        }
        ck_ = h_;
        cs_ = CipherState{};
    }

    void mix_hash(std::span<const std::byte> data) noexcept { h_ = hash(h_, data); }

    void mix_key(std::span<const std::byte> ikm) noexcept {
        Hash64 next_ck{}, temp_k{};
        hkdf2(ck_, ikm, next_ck, temp_k);
        ck_ = next_ck;
        Key32 k{};
        std::memcpy(k.data(), temp_k.data(), kKeyLen);  // HASHLEN == 64: truncate to 32 (Noise §5.2)
        cs_.initialize_key(k);
        wipe(next_ck);
        wipe(temp_k);
        wipe(k);
    }

    [[nodiscard]] bool encrypt_and_hash(std::span<const std::byte> pt, Bytes& out) {
        const std::size_t base = out.size();
        if (!cs_.encrypt_with_ad(h_, pt, out)) return false;
        mix_hash(std::span<const std::byte>(out.data() + base, out.size() - base));
        return true;
    }

    [[nodiscard]] bool decrypt_and_hash(std::span<const std::byte> ct, Bytes& out) {
        if (!cs_.decrypt_with_ad(h_, ct, out)) return false;
        mix_hash(ct);
        return true;
    }

    // Split (Noise §5.2): the two transport keys, truncated to 32 bytes. c1 encrypts initiator->responder.
    void split(CipherState& c1, CipherState& c2) noexcept {
        Hash64 t1{}, t2{};
        hkdf2(ck_, std::span<const std::byte>{}, t1, t2);
        Key32 k1{}, k2{};
        std::memcpy(k1.data(), t1.data(), kKeyLen);
        std::memcpy(k2.data(), t2.data(), kKeyLen);
        c1.initialize_key(k1);
        c2.initialize_key(k2);
        wipe(t1);
        wipe(t2);
        wipe(k1);
        wipe(k2);
    }

    [[nodiscard]] const Hash64& handshake_hash() const noexcept { return h_; }

private:
    Hash64 h_{};
    Hash64 ck_{};
    CipherState cs_;
};

// ---- HandshakeState for the IK pattern (Noise §7.4 / §8) -------------------------------------------
//
//   IK:   <- s
//         ...
//         -> e, es, s, ss
//         <- e, ee, se
//
// The initiator knows the responder's static key in advance (here: PINNED by the application), the responder
// learns the initiator's static key from message 1 (encrypted to the responder). Message 1 is
// 32 + (32 + 16) + payload + 16 bytes; message 2 is 32 + payload + 16 bytes — both fixed for a fixed payload.
inline constexpr std::string_view kSuiteName = "Noise_IK_25519_ChaChaPoly_BLAKE2b";
inline constexpr std::size_t kMsg1Overhead = kDhLen + (kDhLen + kTagLen) + kTagLen;  // 96
inline constexpr std::size_t kMsg2Overhead = kDhLen + kTagLen;                       // 48

enum class Role : std::uint8_t { Initiator, Responder };

class IkHandshake {
public:
    // Fills `sk` with a fresh ephemeral private key. The default (empty) uses libsodium's CSPRNG. A
    // deterministic source exists ONLY so the official test vectors, which fix the ephemerals, can be
    // replayed; production code never passes one (reusing an ephemeral destroys the handshake's security).
    using EphemeralSource = std::function<void(Key32& sk)>;

    // `rs_pub` is the responder's static public key; required for the initiator, ignored for the responder.
    IkHandshake(Role role, std::span<const std::byte> prologue, const KeyPair& s, const Key32* rs_pub,
                EphemeralSource ephemeral = {})
        : role_(role), s_(s), ephemeral_(std::move(ephemeral)) {
        ss_.initialize(kSuiteName);
        ss_.mix_hash(prologue);
        if (role == Role::Initiator) {
            if (rs_pub) rs_ = *rs_pub;
            have_rs_ = rs_pub != nullptr;
            ss_.mix_hash(rs_);  // pre-message "<- s": the initiator hashes the responder's static
        } else {
            ss_.mix_hash(s_.pk);  // ... and the responder hashes its own
        }
    }
    ~IkHandshake() {
        wipe(s_.sk);
        wipe(e_.sk);
    }
    IkHandshake(const IkHandshake&) = delete;
    IkHandshake& operator=(const IkHandshake&) = delete;

    // ---- initiator ----
    // `-> e, es, s, ss` + payload. Appends the message to `out`.
    [[nodiscard]] bool write_msg1(std::span<const std::byte> payload, Bytes& out) {
        if (role_ != Role::Initiator || stage_ != 0 || !have_rs_) return fail();
        gen_ephemeral();
        out.insert(out.end(), e_.pk.begin(), e_.pk.end());
        ss_.mix_hash(e_.pk);
        Key32 d{};
        if (!dh(e_.sk, rs_, d)) {  // es
            wipe(d);
            return fail();
        }
        ss_.mix_key(d);
        if (!ss_.encrypt_and_hash(s_.pk, out)) return fail();  // s
        if (!dh(s_.sk, rs_, d)) return fail();                 // ss
        ss_.mix_key(d);
        wipe(d);
        if (!ss_.encrypt_and_hash(payload, out)) return fail();
        stage_ = 1;
        return true;
    }

    // `<- e, ee, se` + payload. The decrypted payload is appended to `payload`.
    [[nodiscard]] bool read_msg2(std::span<const std::byte> in, Bytes& payload) {
        if (role_ != Role::Initiator || stage_ != 1 || in.size() < kMsg2Overhead) return fail();
        std::memcpy(re_.data(), in.data(), kDhLen);
        ss_.mix_hash(re_);
        Key32 d{};
        if (!dh(e_.sk, re_, d)) return fail();  // ee
        ss_.mix_key(d);
        if (!dh(s_.sk, re_, d)) return fail();  // se (initiator static x responder ephemeral)
        ss_.mix_key(d);
        wipe(d);
        if (!ss_.decrypt_and_hash(in.subspan(kDhLen), payload)) return fail();
        stage_ = 2;
        return true;
    }

    // ---- responder ----
    // Reads message 1. On success `remote_static()` is the initiator's static key and the decrypted payload is
    // appended to `payload`. FALSE means the message did not authenticate (a prober without our static key,
    // or a corrupted/forged message): the caller must stay SILENT.
    [[nodiscard]] bool read_msg1(std::span<const std::byte> in, Bytes& payload) {
        if (role_ != Role::Responder || stage_ != 0 || in.size() < kMsg1Overhead) return fail();
        std::memcpy(re_.data(), in.data(), kDhLen);
        ss_.mix_hash(re_);
        Key32 d{};
        if (!dh(s_.sk, re_, d)) return fail();  // es (responder static x initiator ephemeral)
        ss_.mix_key(d);
        Bytes rs_plain;
        if (!ss_.decrypt_and_hash(in.subspan(kDhLen, kDhLen + kTagLen), rs_plain) || rs_plain.size() != kDhLen)
            return fail();
        std::memcpy(rs_.data(), rs_plain.data(), kDhLen);
        have_rs_ = true;
        if (!dh(s_.sk, rs_, d)) return fail();  // ss
        ss_.mix_key(d);
        wipe(d);
        if (!ss_.decrypt_and_hash(in.subspan(kDhLen + kDhLen + kTagLen), payload)) return fail();
        stage_ = 1;
        return true;
    }

    // `<- e, ee, se` + payload.
    [[nodiscard]] bool write_msg2(std::span<const std::byte> payload, Bytes& out) {
        if (role_ != Role::Responder || stage_ != 1) return fail();
        gen_ephemeral();
        out.insert(out.end(), e_.pk.begin(), e_.pk.end());
        ss_.mix_hash(e_.pk);
        Key32 d{};
        if (!dh(e_.sk, re_, d)) return fail();  // ee
        ss_.mix_key(d);
        if (!dh(e_.sk, rs_, d)) return fail();  // se (responder ephemeral x initiator static)
        ss_.mix_key(d);
        wipe(d);
        if (!ss_.encrypt_and_hash(payload, out)) return fail();
        stage_ = 2;
        return true;
    }

    // After both messages. `send` is this side's write direction, `recv` its read direction (Noise §5.2: the
    // initiator writes with c1, the responder with c2).
    [[nodiscard]] bool split(CipherState& send, CipherState& recv) noexcept {
        if (stage_ != 2) return false;
        CipherState c1, c2;
        ss_.split(c1, c2);
        if (role_ == Role::Initiator) {
            send = std::move(c1);
            recv = std::move(c2);
        } else {
            send = std::move(c2);
            recv = std::move(c1);
        }
        return true;
    }

    [[nodiscard]] const Key32& remote_static() const noexcept { return rs_; }
    [[nodiscard]] const Hash64& handshake_hash() const noexcept { return ss_.handshake_hash(); }
    [[nodiscard]] const Key32& local_ephemeral_public() const noexcept { return e_.pk; }

private:
    bool fail() noexcept {
        stage_ = 0xFF;
        return false;
    }
    void gen_ephemeral() {
        if (ephemeral_)
            ephemeral_(e_.sk);
        else
            ::randombytes_buf(e_.sk.data(), e_.sk.size());
        e_.pk = public_from_private(e_.sk);
    }

    Role role_;
    KeyPair s_{};
    KeyPair e_{};
    Key32 rs_{};
    Key32 re_{};
    bool have_rs_ = false;
    std::uint8_t stage_ = 0;
    EphemeralSource ephemeral_;
    SymmetricState ss_;
};

}  // namespace quark::adapters::noise
