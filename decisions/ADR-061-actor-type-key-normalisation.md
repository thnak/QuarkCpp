# ADR-061 — Cross-toolchain actor type-key normalisation (#17 finding; 008 open question 1)

Status: **Accepted, actor part SUPERSEDED by [ADR-062](ADR-062-required-actor-type-name.md)** (2026-10-28; owner accepted the pre-1.0 durable key break and a large refactor).
Implemented in `include/quark/core/metadata.hpp` (`normalize_type_name`, `type_name_hash`); evidence in
section 9. Governs spec 008 §Type identity, 016, 010.

## 1. Problem (measured, not read)
An actor's `TypeKey` is `fnv1a(canonical_type_name<A>())` folded with each protocol message's key
(`metadata.hpp` `fold_protocol`). The message part is name-free (016 fingerprint) and matches on every
toolchain; the **name part is the compiler's own spelling** (`__PRETTY_FUNCTION__` / `__FUNCSIG__`).
That key is the first half of the wire key `wire_key(type_key_of<A>(), type_key_of<M>())`
(`distribution.hpp` `register_remote` / `send_remote`) and of every `ActorId`.

Reproduced on a real Windows/x86-64 (clang++ 22) <-> Linux/x86-64 (g++ 15.2) pair
(`tests/netem/mixed_os_node.cpp`, MIXED-OS-AND-BULK-TRANSFER.md "Real cross-OS run"):

| actor declared in | Responder key (Windows clang) | Responder key (Linux gcc) | remote `tell` |
|---|---|---|---|
| anonymous namespace | `f55bc1c186fe9998` | `ff03938368e56eea` | frames arrive (tagged), `req_handled=0` |
| named namespace `mixed_os` | `661cf3b06fce35b0` | `661cf3b06fce35b0` | all dispatched, PASS |

So for **named** types Clang and GCC already agree; the divergence is in how each compiler spells things
the C++ name alone does not fix:
1. **Anonymous namespace**: GCC `{anonymous}`, Clang `(anonymous namespace)`, MSVC `` `anonymous namespace' ``.
2. **MSVC elaborators**: `__FUNCSIG__` keeps `struct ` / `class ` / `enum ` / `union ` before every type
   name (including inside template arguments); GCC/Clang never print them. Every `cl.exe` actor differs
   from every GCC/Clang actor. (Recorded in OpenQuestions.md and the conformance test; not re-run here,
   this host builds Windows with clang++.)
3. **Template-argument spacing**: GCC/Clang `Foo<int, int>`, MSVC `Foo<int,int>`.
4. **Builtin spellings** (`long unsigned int` / `unsigned long` / `unsigned __int64`) and **inline
   standard-library namespaces** (`std::__cxx11::`, `std::__1::`) — see non-goals.

Hypothesis to confirm in section 6 (E1): the "GCC and Clang diverge for the identical type" entry in
OpenQuestions.md (golden `0xc1a5...` vs `0xba4a...`) is item 1 above, because that test's fixture
`ConformanceTag` lives in an anonymous namespace (`tests/metadata_typekey_conformance_test.cpp:34-74`).
If so, GCC/Clang agreement for ordinary named types is the common case, and the recorded "diverge
pairwise on all three" is narrower than stated.

Consequence today: a mixed-toolchain cluster, or shared durable logs across toolchains, silently fails
remote dispatch for affected actors (receiver decodes nothing, handler never runs; no error surfaces).

## 2. Options
- **A. Normalise the name before hashing.** Make the three spellings produce one canonical string for
  the C++ name. Zero user effort; keys of types that already agree do not change.
- **B. Developer-assigned identity.** `static constexpr std::string_view quark_type_name = "..."` (or a
  `std::uint64_t quark_actor_key`) on the actor, mirroring the existing `quark_type_salt` (ADR-050). Always
  correct, survives renames and moves between namespaces, but opt-in and easy to forget.
- **C. Status quo + document** "build every node with one compiler family". Cheapest; leaves the silent
  failure for anyone who mixes (AeroCoWorker's Windows workstations + Linux hubs is exactly that case).

## 3. Decision: A as the default, B as the explicit override — a policy, not a fixed choice
1. `quark_type_name` (B) wins when present: the key folds that string and nothing compiler-derived.
   This is the recommended spelling for any actor that is persisted or crosses a toolchain boundary.
2. Otherwise `canonical_type_name` is **normalised to the GCC/Clang spelling** before hashing:
   a. strip the elaborators `struct ` / `class ` / `enum ` / `union ` at token boundaries (start, after
      `<`, `,`, space, `(`, `::`);
   b. map the three anonymous-namespace spellings to the single token `(anonymous namespace)` (Clang's,
      so Clang keys are unchanged);
   c. insert one space after each `,` that lacks one (GCC/Clang already have it, so they are unchanged).
   Done in a `constexpr` pass that folds bytes directly into FNV-1a (no intermediate buffer; no
   pointers stored), so it stays usable in constant expressions like today.
3. Keep the **startup collision scan** (008) as the backstop: two actors that normalise to one key still
   fail startup validation.
4. No change to Described message keys (already portable) and none to the hot path: this is constexpr /
   cold-path identity only; the 0-alloc, 0-RTTI, <=64 B descriptor invariants are untouched.

## 4. Compatibility and migration (the cost)
Keys change only for: GCC anonymous-namespace types; every `cl.exe` type (elaborators); MSVC types with
multi-argument templates. Keys of **named GCC/Clang** types, the common case, are unchanged, to be proven
by the existing Clang golden staying equal (E2). Where keys do change:
- persisted `ActorId`s and any event log / snapshot header carrying an actor key become unreadable under
  the new key. **Same precedent as ADR-050** (pre-1.0 durable break, noted in README). Proposed: accept it,
  and add `quark_type_name` as the migration path for anyone who must keep an old durable identity.
- wire: a node on the old normalisation and one on the new compute different keys, so the cluster must
  upgrade together or use `quark_type_name`; this lands inside the N / N-1 window work (ADR-053) as a
  protocol note, not a new wire field.

## 5. Non-goals
- **Builtin spellings (Tier 3)**: `unsigned long` is 32-bit on Windows and 64-bit on Linux, so these are
  not one type; normalising the spelling would hide a real layout difference. Actors templated over such
  types must use `quark_type_name`. (Described messages are unaffected: they use layout, not names.)
- Inline standard namespaces (`std::__cxx11::`, `std::__1::`): not stripped in this ADR; actor types that
  name std types in template arguments should use `quark_type_name`. Revisit if E5 shows it is common.
- Renames: A still changes the key if the C++ name changes; only B is rename-stable.

## 6. Experiments that accept or reject this (each falsifiable)
- **E1 (confirm the diagnosis)**: extend `tests/metadata_typekey_conformance_test.cpp` with a fixture in a
  NAMED namespace next to the anonymous one. Claim: named fixture is identical on GCC and Clang today;
  anonymous differs. If the named one differs, the diagnosis (section 1) is wrong — stop.
- **E2 (no drift for the common case)**: the Clang golden for the named fixture is unchanged after the
  patch, and so is the GCC golden for it. Any change rejects A as written.
- **E3 (convergence)**: after the patch, named + anonymous + two-argument-template fixtures produce ONE
  key on Clang, GCC and **real `cl.exe`** (the CI `test-windows` msvc leg already builds it). Failure on
  MSVC means rule 2a/2b is incomplete.
- **E4 (end to end)**: rerun `tests/netem/run_mixed_os.sh` with the actors back in the anonymous namespace;
  must PASS Windows <-> Linux. Add the `cl.exe` <-> g++ pair if a Windows MSVC build of
  `mixed_os_node` is available.
- **E5 (override)**: actor with `quark_type_name` has the same key renamed and across toolchains;
  startup scan still rejects two actors sharing one `quark_type_name`.
- **E6 (compile-time)**: normalisation is a constant expression (`static_assert` the key), and the metadata
  startup bench / `-Wall -Wextra` stay clean on GCC 14, Clang 22, MSVC.

## 7. Spec / doc updates if accepted
008 §Type identity (normalisation rules + `quark_type_name`); OpenQuestions.md item 1 (answered, with the
corrected scope from E1); README ADR index; MIXED-OS-AND-BULK-TRANSFER.md (drop the "same compiler family"
workaround); VERIFICATION.md (new conformance rows). Update the conformance test goldens in the same
change that lands the code (spec first, per CLAUDE.md).

## 8. Open questions for the owner
1. Accept the pre-1.0 durable key break for `cl.exe` and GCC-anonymous types (as ADR-050 did), or require a
   migration shim (`QUARK_LEGACY_TYPE_KEYS`)?
2. Should `quark_type_name` be **required** (startup error) for actors with `Persistent` state, since
   those are the ones whose key must outlive a refactor?
3. Is a Windows `cl.exe` <-> Linux g++ pair a supported deployment? If not, rule 2a can be deferred and
   only 2b/2c (clang <-> gcc) shipped first.

## 9. Outcome (implemented and measured, 2026-10-28)
Owner decisions: the pre-1.0 durable key break is **accepted** (no `QUARK_LEGACY_TYPE_KEYS` shim);
`quark_type_name` stays **optional** (recommended, not enforced, for persistent actors — open question 2
left as a recommendation); all three normalisation rules (2a elaborators, 2b anonymous namespace, 2c comma
spacing) shipped, so the `cl.exe` <-> GCC/Clang pair is covered (open question 3).

| Experiment | Result |
|---|---|
| E1 diagnosis | Confirmed. The named fixture agreed between GCC and Clang before the change; only the anonymous-namespace one diverged. The recorded "GCC vs Clang diverge pairwise" was the anonymous-namespace effect, not a general name divergence. |
| E2 no drift | Clang's anonymous-namespace golden is byte-identical before and after (`0xba4a66eb2b3e6ff3`); Windows-clang `Responder` key unchanged (`f55bc1c186fe9998`). |
| E3 convergence | All four Tier 2 fixtures (anonymous, named, two-argument template, `quark_type_name`) are identical on Clang 22 (Windows), Clang 21, g++ 15.2 (Linux) and **real `cl.exe` 14.51** (Windows). `0xba4a...` (anon), `0x4202adf0189cbfa1` (named), `0x28f38acdf8c467cb` (pair), `0x1a4e0103acebb260` (pinned). |
| E4 end to end | Windows (clang) <-> Linux (g++), actors back in an anonymous namespace: PASS, `req_handled=50` (was 0). Linux `Responder` key now `f55bc1c186fe9998`, equal to Windows (was `ff03938368e56eea`). |
| E5 override | `quark_type_name`: same pinned name + protocol => same key regardless of C++ name; different protocol => distinct; the Strict startup scan rejects the second actor (`metadata_typekey_test`). |
| E6 constexpr | `static_assert(type_name_hash<T>() == type_key_of<T>().value)` compiles on all four toolchains, `-Wall -Wextra` clean. |
| Regression | Full suite 241/241 on Windows (clang). |

Tests: `tests/metadata_typekey_conformance_test.cpp` (one golden per fixture on every toolchain, plus
compiler-independent checks that feed the GCC, Clang and MSVC spellings through the normaliser directly),
`tests/metadata_typekey_test.cpp` (E5). The retired per-compiler goldens were GCC `0xc1a54de107d9d89f` and
MSVC `0x2d4c989423ddf201`.

### Not done / residual
- Not run under TSan/ASan for this change (constexpr, cold path only); only the Linux g++/clang builds above.
- Linux/arm64 and macOS not exercised; builtin spellings and inline std namespaces remain non-goals.
- MSVC spelling of an anonymous-namespace type was handled from the documented forms and verified through
  the conformance test on real `cl.exe`; the exact `` `anonymous namespace' `` punctuation of other MSVC
  versions is unverified (rules also accept `` `anonymous-namespace' ``).
- Durable break: persisted `ActorId`s / event-log headers for `cl.exe` and GCC-anonymous-namespace types
  from before this change no longer match. Use `quark_type_name` to pin an identity going forward.

> **Update (ADR-062):** actors no longer use the normalised compiler name at all; `quark_type_name` is
> required on every actor. The normaliser in this ADR now applies only to non-actor, non-Described, local-only
> types (e.g. an authorizer's message key).
