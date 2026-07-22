// Tests 020-Security §4 — the `SecretSource` seam and its two std-only default adapters (environment
// `QUARK_SECRET_*`, and a mounted-secret file reader), plus `resolve_secrets` over a `SecurityConfig`'s
// REFERENCES. The config holds names; resolution produces zeroizing `Secret`s; a named-but-missing
// secret fails fast (errc::not_found) — it never runs with a silently-absent key.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <string>

#include "tmp_dir_util.hpp"

#include "quark/core/secret.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
bool bytes_equal(std::span<const std::byte> b, std::string_view s) {
    return b.size() == s.size() && std::memcmp(b.data(), s.data(), s.size()) == 0;
}
// MSVC's CRT has no POSIX setenv; _putenv_s is its equivalent (always-overwrite, matching every call
// site in this file passing setenv's overwrite=1).
void set_env(const char* name, const char* value) {
#if defined(_MSC_VER)
    ::_putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}
}  // namespace

int main() {
    bool ok = true;

    // --- Environment adapter: QUARK_SECRET_<name>. -------------------------------------------------
    {
        set_env("QUARK_SECRET_cluster_key", "s3kr1t-cluster");
        EnvSecretSource env;
        auto s = env.get("cluster_key");
        check(s.has_value(), "env: resolves QUARK_SECRET_cluster_key", ok);
        check(s && bytes_equal(s->bytes(), "s3kr1t-cluster"), "env: value matches", ok);

        auto miss = env.get("absent_key");
        check(!miss.has_value() && miss.error().code == errc::not_found, "env: missing ⇒ not_found", ok);
    }

    // --- File adapter: <dir>/<name>, trailing newline stripped. ------------------------------------
    {
        const std::string base = quark::test::make_temp_dir("quark_sec_test_");
        const std::string path = base + "/quark_sec_test_at_rest_key";
        {
            std::ofstream f(path, std::ios::binary);
            f << "file-mounted-key\n";  // trailing newline (mounted-secret convention)
        }
        FileSecretSource fs(base);
        auto s = fs.get("quark_sec_test_at_rest_key");
        check(s.has_value(), "file: resolves the mounted secret", ok);
        check(s && bytes_equal(s->bytes(), "file-mounted-key"), "file: trailing newline stripped", ok);
        std::remove(path.c_str());

        auto miss = fs.get("no_such_file");
        check(!miss.has_value() && miss.error().code == errc::not_found, "file: missing ⇒ not_found", ok);
    }

    // --- resolve_secrets over a SecurityConfig's references (013 config surface, 020 §4). ----------
    {
        set_env("QUARK_SECRET_ck", "CK");
        set_env("QUARK_SECRET_tk", "TK");
        EnvSecretSource env;

        SecurityConfig cfg;
        cfg.cluster_key_ref = "ck";
        cfg.transport_key_ref = "tk";
        // at_rest_key_ref intentionally left empty ⇒ skipped, no error.
        auto resolved = resolve_secrets(cfg, env);
        check(resolved.has_value(), "resolve: both configured references resolve", ok);
        check(resolved && bytes_equal(resolved->cluster_key.bytes(), "CK"), "resolve: cluster key", ok);
        check(resolved && bytes_equal(resolved->transport_key.bytes(), "TK"), "resolve: transport key", ok);
        check(resolved && resolved->at_rest_key.empty(), "resolve: empty reference ⇒ empty secret", ok);

        // A configured-but-missing reference fails fast.
        SecurityConfig bad;
        bad.cluster_key_ref = "does_not_exist";
        auto r = resolve_secrets(bad, env);
        check(!r.has_value() && r.error().code == errc::not_found,
              "resolve: a named-but-missing secret fails fast (not_found)", ok);
    }

    std::printf("security_secret_source_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
