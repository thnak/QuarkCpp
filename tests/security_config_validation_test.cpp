// Tests 020-Security §"013 SecurityMode validation" (008) — the config invariant: under
// SecurityMode::Strict, a MULTI-NODE cluster on the plaintext dev transport is a startup Validation
// FAILURE (errc::validation), consistent with the `result<…>` model of validate_engine_config. Under
// Off (or a single node, or a secure transport) it is allowed.
//
// CONTROL (adversarial, must FIRE): the exact forbidden combination (Strict + distributed + plaintext)
// returns an error; flipping ANY one of the three conditions makes it pass — isolating the rule.
#include <cstdio>

#include "quark/core/engine_config.hpp"
#include "quark/core/security.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
}  // namespace

int main() {
    bool ok = true;

    // --- CONTROL: the forbidden combination is REJECTED. -------------------------------------------
    {
        const auto r = validate_security(SecurityMode::Strict, /*cluster*/ 3, /*plaintext*/ true);
        check(!r.has_value() && r.error().code == errc::validation,
              "CONTROL: Strict + multi-node + plaintext ⇒ errc::validation", ok);
    }

    // --- Flip each condition ⇒ allowed (isolating that all three are required to trip the rule). -----
    check(validate_security(SecurityMode::Off, 3, true).has_value(),
          "Off + multi-node + plaintext ⇒ allowed (dev posture)", ok);
    check(validate_security(SecurityMode::Strict, 1, true).has_value(),
          "Strict + single node + plaintext ⇒ allowed (no cross-node boundary)", ok);
    check(validate_security(SecurityMode::Strict, 3, false).has_value(),
          "Strict + multi-node + SECURE transport ⇒ allowed", ok);

    // --- The setting rides on EngineConfig (013) and defaults Off (zero-cost when unused). ----------
    {
        EngineConfig cfg{};
        check(cfg.security_mode == SecurityMode::Off, "EngineConfig defaults SecurityMode::Off", ok);

        auto built = ConfigBuilder{}.workers(1).shards(1).security_mode(SecurityMode::Strict).build();
        check(built.has_value() && built->security_mode == SecurityMode::Strict,
              "ConfigBuilder carries the SecurityMode (a reference/posture, not a secret)", ok);
    }

    // --- The end-to-end startup check a host would run: combine engine-config validation with the
    //     security-posture validation, both in the same result<> model (008). -----------------------
    {
        auto cfg = ConfigBuilder{}.workers(1).shards(1).security_mode(SecurityMode::Strict).build();
        check(cfg.has_value(), "base engine config validates", ok);
        const auto sec = validate_security(cfg->security_mode, /*cluster*/ 4, /*plaintext*/ true);
        check(!sec.has_value(), "startup rejects the distributed plaintext Strict deployment", ok);
    }

    std::printf("security_config_validation_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
