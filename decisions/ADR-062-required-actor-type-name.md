# ADR-062 — Required explicit actor `quark_type_name`: one identity mechanism on every platform (008; supersedes the actor part of ADR-061)

Status: **Accepted** (2026-10-28; owner: "make it required, all platforms share one mechanism").
Implemented in `include/quark/core/metadata.hpp` (`has_quark_type_name`, `fold_protocol`, `raw_type_key`,
`type_label`); governs spec 008 §Type identity, CONVENTIONS.md "Naming & files", 016, 010.

## 1. Why ADR-061 was not enough
ADR-061 normalised the compiler-derived name so GCC, Clang and MSVC agree for ordinary C++ names, and made
`quark_type_name` an opt-in override. That left two compiler-derived inputs in every actor key:
1. the **name spelling** (normalised, but still a per-compiler string with known holes: builtin spellings,
   inline std namespaces, anything a future compiler prints differently); and
2. the **protocol fold** over every message, including non-Described messages whose key is again a
   compiler-derived name (`Protocol<std::size_t>` hashes `long unsigned int` / `unsigned long` /
   `unsigned __int64` differently per toolchain — measured, Tier 3).

"Make it required, all platforms share one mechanism" removes both: an actor's identity comes from ONE
place that no compiler touches.

## 2. Decision
1. **Every actor MUST declare** `static constexpr std::string_view quark_type_name = "...";`. A missing or
   empty name is a compile error (`static_assert` in `raw_type_key`, message cites ADR-062).
2. **Actor key** = `FNV-1a(quark_type_name)`, folded in protocol order with the 016 fingerprint of each
   **Described** message in the protocol. Non-Described messages are local-only (never on the wire, never in
   a durable record) and contribute nothing. An actor with no `protocol` is exactly `FNV-1a(name)`.
3. `protocol_keys_distinct` (the #18 same-layout check) now compares Described messages only.
4. **Non-actor types** keep ADR-061's rules unchanged: Described => 016 fingerprint; `quark_type_name` if
   declared; otherwise the *normalised* compiler name. That key is process-local (authorizer message keys,
   labels) and must not be persisted or sent.
5. **Metrics labels** use the pinned name (`detail::type_label<A>()`), so the Prometheus `type` label is the
   same string on every platform instead of a compiler spelling.
6. Two actors with the same key are still rejected by the Strict startup collision scan (008).

Not required: a name on a *message* type (a Described message is identified by its fingerprint; adding a
name there would be a second mechanism), and a name on local-only non-Described messages.

## 3. Consequences
- **Durable / wire break (accepted, pre-1.0, same precedent as ADR-050/061):** every actor key changes
  (a name-derived hash became a pinned-name hash; non-Described messages left the fold), so persisted
  `ActorId`s, event-log/snapshot headers carrying an actor key and any mixed-version cluster do not
  interoperate across this change. There is no migration shim.
- **Migration is mechanical and was done in-tree:** 181 actor declarations across 118 files (tests, benches,
  samples, the internal broker) received `quark_type_name = "<area>.<file>.<Struct>"`. Names in this repo's
  own tests/benches/samples are unique per program but are not a stable public contract; production actors
  should use a deliberate, dotted, application-level name (`"app.billing.Ledger"`).
- **The name is the contract:** renaming or moving the C++ type no longer changes the key; changing the
  *string* does. Two different actors must not share a string.
- Rule 4 of ADR-061 (normalisation) is still needed for non-actor local types, so that code stays, but it no
  longer sits on any cross-node identity path.

## 4. Evidence (measured 2026-10-28)
| Check | Result |
|---|---|
| Tier 4 actor goldens, `tests/metadata_typekey_conformance_test.cpp` | Identical on **Clang 22 (Windows), Clang 21 (Linux), g++ 15.2 (Linux), real `cl.exe` 14.51 (Windows)**: anonymous-namespace actor with `size_t` in its protocol `0xb3d61babaac77327`, named actor `0x32464213a5ce9f09`, no-protocol actor `0x92462dbdb69011ea`. In the same run the raw Tier 3 `Wrap<size_t>` key still differs per compiler (`0xb49a...` Clang, `0xbeb6...` MSVC, `0x3de9...` GCC): the actor key no longer inherits that divergence. |
| `tests/metadata_typekey_test.cpp` | Same name + protocol => same key regardless of C++ name; a Described message folds in; a non-Described one does not; different names => different keys; Strict startup scan rejects a duplicate identity. |
| Compile-time gate, `tests/CMakeLists.txt` `try_compile` | Positive control (named actor) compiles; the unnamed actor is rejected **and** the diagnostic contains "ADR-062". Runs at every configure. |
| Real Windows (clang) <-> Linux (g++) cluster, `tests/netem/run_mixed_os.sh` | PASS with the new keys (`Responder=8893364aca69835f` on both), `req_handled=50`, tagged fallback 100/0. |
| Full suite | 241/241 on Windows after a full rebuild, with the build exit status checked (an earlier run in this change used stale binaries and is not counted). |

### Not done / residual
- No TSan/ASan run for this change (constexpr, cold path); no Linux full-suite run (only the conformance
  test and `mixed_os_node` were built there); Linux/arm64 and macOS not exercised.
- A live `cl.exe` <-> g++ *cluster* run was not done; key derivation on real `cl.exe` was.
- Documentation snippets in README, 005, CONVENTIONS, wiki and samples/README were updated; spec snippets in the
  other RFCs (001, 004, 007, 012, 017, 024, 025) still show actors without a name and should be swept.
- Names are not validated for charset/length beyond non-empty; uniqueness is enforced by the startup scan, not
  at compile time (a compile-time cross-TU check is impossible without a registry).
