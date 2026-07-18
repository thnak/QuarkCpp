// Implements 020-Security §4 (Secrets) — the `SecretSource` seam + the zeroizing `Secret` type, plus
// the two std-only default adapters (environment `QUARK_SECRET_*`, and a file/mounted-secret reader).
//
// THE INVARIANT (020 §4): secret material (cluster keys, TLS keys, store credentials) is NEVER a
// literal in `EngineConfig` (013 carries secret *references* — names — only) and NEVER reaches a log or
// metric (009). `Secret` enforces two mechanical guards:
//   * it ZEROIZES its buffer on destruction (a stale key does not linger in freed heap / a core dump);
//   * it is NON-COPYABLE and has NO conversion to `std::string` — you cannot accidentally hand secret
//     bytes to a logger, an ostream, or a formatter. You get a `std::span<const std::byte>` view whose
//     lifetime is the `Secret`'s, and nothing more.
//
// DEFERRED adapters (020 §4, behind this same seam — NOT implemented here, they are OS-keystore work):
//   macOS Keychain, Windows DPAPI/CNG, Linux kernel keyring. These are `SecretSource` adapters, not a
//   PAL concern (the PAL stays about compute/IO, 019). The env + file readers below cover the
//   datacenter default (mounted secret files / injected env), which is the Linux/x86-64 verified target.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "quark/core/error.hpp"

namespace quark {

namespace detail {
// A best-effort secure zero the optimizer may not elide. `std::memset` on a to-be-freed buffer is a
// classic dead-store the compiler removes; touching each byte through a volatile pointer prevents that.
// (Not a defense against a determined attacker with memory access — 020 non-goals — but it removes the
// casual "key still sits in freed heap / core dump" exposure the spec calls out.)
inline void secure_zero(void* p, std::size_t n) noexcept {
    auto* v = static_cast<volatile unsigned char*>(p);
    while (n-- > 0) *v++ = 0;
}
}  // namespace detail

// A resolved secret's bytes. Owns a heap buffer that is zeroized on destruction. Move-only: copying a
// secret would multiply the number of live plaintext copies (and each would have to be zeroized), so
// the type simply forbids it. There is intentionally NO `std::string` conversion and NO `operator<<`.
class Secret {
public:
    Secret() = default;

    // Take ownership of already-resolved bytes (the source path). The vector's storage becomes the
    // secret buffer; the source should not retain a second copy.
    explicit Secret(std::vector<std::byte> bytes) noexcept : buf_(std::move(bytes)) {}

    // Build from a byte range (copies once into the owned buffer).
    Secret(const std::byte* data, std::size_t len) : buf_(data, data + len) {}

    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;

    Secret(Secret&& o) noexcept : buf_(std::move(o.buf_)) { o.buf_.clear(); }
    Secret& operator=(Secret&& o) noexcept {
        if (this != &o) {
            wipe();
            buf_ = std::move(o.buf_);
            o.buf_.clear();
        }
        return *this;
    }

    ~Secret() { wipe(); }

    // A read-only view of the material, valid for this Secret's lifetime. This is the ONLY accessor —
    // there is deliberately no owning-string getter, so secret bytes cannot be casually copied out.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return std::span<const std::byte>(buf_.data(), buf_.size());
    }
    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }
    [[nodiscard]] bool empty() const noexcept { return buf_.empty(); }

private:
    void wipe() noexcept {
        if (!buf_.empty()) detail::secure_zero(buf_.data(), buf_.size());
        buf_.clear();
    }
    std::vector<std::byte> buf_;
};

// Build a Secret from a string_view of characters (the env/file readers use this). Not a member of
// Secret to keep Secret free of any `std::string`-shaped surface.
[[nodiscard]] inline Secret make_secret(std::string_view chars) {
    std::vector<std::byte> b(chars.size());
    std::memcpy(b.data(), chars.data(), chars.size());
    return Secret(std::move(b));
}

// ============================================================================================
// The SecretSource seam (020 §4). Resolution happens at startup, off the hot path; a miss is a
// `result` error (errc::not_found), never a throw. Adapters (env/file here; OS keystores DEFERRED)
// all model this one interface, so `EngineConfig`'s secret *references* resolve identically.
// ============================================================================================
class SecretSource {
public:
    virtual ~SecretSource() = default;

    // Resolve the secret named `name` (013 reference) into a zeroizing `Secret`. `not_found` if the
    // name is absent; `unavailable` if the backing store cannot be read.
    [[nodiscard]] virtual result<Secret> get(std::string_view name) = 0;
};

// ---------------------------------------------------------------------------------------------
// Default adapter 1 — environment variables, `QUARK_SECRET_<NAME>` (020 §4). The reference "cluster_key"
// resolves the env var `QUARK_SECRET_cluster_key`. Cold path; the value is copied into a `Secret` and
// the process env string is NOT zeroized (it is the OS's copy, outside our control — mounted-file
// secrets, adapter 2, avoid leaving plaintext in the environment block).
// ---------------------------------------------------------------------------------------------
class EnvSecretSource final : public SecretSource {
public:
    explicit EnvSecretSource(std::string_view prefix = "QUARK_SECRET_") : prefix_(prefix) {}

    [[nodiscard]] result<Secret> get(std::string_view name) override {
        std::string var;
        var.reserve(prefix_.size() + name.size());
        var.append(prefix_);
        var.append(name);
        const char* v = std::getenv(var.c_str());
        if (v == nullptr) return fail(errc::not_found, "secret not in environment");
        return make_secret(std::string_view(v));
    }

private:
    std::string prefix_;
};

// ---------------------------------------------------------------------------------------------
// Default adapter 2 — mounted-secret files (020 §4: "a file/mounted-secret reader"). The reference
// resolves to `<dir>/<name>` (the Kubernetes / systemd-credentials convention). Reads the whole file
// as opaque bytes into a `Secret`. A trailing newline is stripped (mounted secrets frequently carry
// one) — otherwise the bytes are verbatim.
// ---------------------------------------------------------------------------------------------
class FileSecretSource final : public SecretSource {
public:
    explicit FileSecretSource(std::string dir) : dir_(std::move(dir)) {}

    [[nodiscard]] result<Secret> get(std::string_view name) override {
        std::string path = dir_;
        if (!path.empty() && path.back() != '/') path.push_back('/');
        path.append(name);
        std::ifstream f(path, std::ios::binary);
        if (!f) return fail(errc::not_found, "secret file not found");
        std::vector<std::byte> bytes;
        char c;
        while (f.get(c)) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        if (!bytes.empty() && bytes.back() == std::byte{'\n'}) bytes.pop_back();
        return Secret(std::move(bytes));
    }

private:
    std::string dir_;
};

// ============================================================================================
// The 013 config-surface companion holding secret REFERENCES (names), not values (020 §4). Kept
// SEPARATE from the frozen, trivially-copyable `EngineConfig` so that struct keeps its aggregate-init
// contract — the invariant is "references, not values, resolved at startup", wherever on the config
// surface the reference sits. An empty reference means "not configured" (resolution is skipped).
// ============================================================================================
struct SecurityConfig {
    std::string cluster_key_ref;    // reference name for the cluster / pre-shared key (020 §1)
    std::string transport_key_ref;  // reference name for the node transport identity key (020 §2)
    std::string at_rest_key_ref;    // reference name for the at-rest KEK / data key (020 §5)
};

// A bundle of resolved secrets. Move-only (each Secret is move-only + zeroizing). Absent references
// yield an empty Secret. Resolution is a cold startup step through the `SecretSource` seam — the
// resolved material lives HERE (zeroizing), never back in a config struct.
struct ResolvedSecrets {
    Secret cluster_key;
    Secret transport_key;
    Secret at_rest_key;
};

// Resolve every non-empty reference in `cfg` through `src`. A named-but-missing secret is a startup
// failure (errc::not_found) — fail fast rather than run with a silently-absent key.
[[nodiscard]] inline result<ResolvedSecrets> resolve_secrets(const SecurityConfig& cfg,
                                                             SecretSource& src) {
    ResolvedSecrets out;
    auto load = [&](const std::string& ref, Secret& into) -> result<void> {
        if (ref.empty()) return {};  // not configured — leave empty
        auto s = src.get(ref);
        if (!s) return std::unexpected(s.error());
        into = std::move(*s);
        return {};
    };
    if (auto r = load(cfg.cluster_key_ref, out.cluster_key); !r) return std::unexpected(r.error());
    if (auto r = load(cfg.transport_key_ref, out.transport_key); !r) return std::unexpected(r.error());
    if (auto r = load(cfg.at_rest_key_ref, out.at_rest_key); !r) return std::unexpected(r.error());
    return out;
}

}  // namespace quark
