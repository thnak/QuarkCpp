# Fuzzing (P5)

Quark's untrusted decode paths share one target set in
[`tests/fuzz/fuzz_targets.hpp`](tests/fuzz/fuzz_targets.hpp):

| Path | Codec |
|---|---|
| canonical tagged wire | `decode_tagged` |
| tagless wire (bounds-checked) | `decode_tagless_checked` |
| F17 mixed schema (reused tags) | V1/V2 tagless+tagged + `negotiate` gate |
| SWIM control plane | `detail::decode_control` |
| F17 schema advertisement | `decode_schema_advert` |
| durable snapshot/event payloads | `decode_record` |
| durable migration chain | `read_migrated<EventV2, EventV1>` |
| TCP frame body | `net::decode_frame_body` |
| TCP frame reassembly | `net::FrameStream::feed` (partial/coalesced/oversized prefixes) |
| persistence WAL replay | `FileStore` open/read over random `.qwal` bytes |
| pinned Noise IK Msg1 / Msg2 (issue #50) | the real `PinnedHandshakeEngine`: responder fed arbitrary bytes as Msg1, initiator fed arbitrary bytes as Msg2, the noise-layer initiator and both payload decoders. **Needs `QUARK_WITH_SODIUM=ON`** (compiled out otherwise) |
| pinned wire, anonymous inbound (#50) | `FrameStream` cut into recv() chunks + `net::anonymous_opener_admissible` (exactly one `Authenticate`, `from == to == 0`, at most 1024 B) |
| discovery decode (#50) | base32 label decode/encode, exact-match accept lists and sighting fields, via `LanDiscovery` over the fake `DnsSdPlatform` with arbitrary strings |
| planted-OOB control (#50) | `control_msg1_parse_copy`, a copy of the Msg1 prefix parse with an overread behind `QUARK_FUZZ_PLANT_OOB` |

The pinned targets are in [`tests/fuzz/fuzz_pinned_targets.hpp`](tests/fuzz/fuzz_pinned_targets.hpp). They abort on a
violated property, not only on a sanitizer trap: a refusing responder emits no bytes; a completing one only for an honest
payload; the initiator never completes on a forged Msg2; the anonymous gate admits at most one plain opener; discovery
reports only ids that EQUAL an accepted id; the label decoder is the strict inverse of the encoder. For the Noise
targets, input byte 0 (low 2 bits) selects an extra mode: raw, byte patches to a valid Msg1/Msg2, a hostile payload under a
real authenticated Msg1 (bits 4-7 repair the key/node/cluster/timestamp fields so later checks are reached), or the input
padded to exactly 154/58 bytes. Static keys are fixed seeds; only an engine's own ephemeral key is random.
Seam added for the anonymous path: `net::anonymous_opener_admissible` and `net::kAnonOpenerMax` in `wire_codec.hpp`,
called by `TcpTransport::do_read` (one rule, shared, no socket needed).

## Two drivers, one target set

1. **`fuzz_decoders_test`** (in the default `ctest`, every platform/compiler). It seeds every path
   from the shared corpus, runs a deterministic random campaign, and does 100 random-byte
   `FileStore` replays.

   ```bash
   ./build/tests/fuzz_decoders_test                 # 50k default
   ./build/tests/fuzz_decoders_test --iters 2000000 # long campaign (ASan/UBSan build recommended)
   ```

   Half of the iterations are random bytes, half are 1-4 havoc mutations of a seed. To include the pinned Noise
   targets configure with `-DQUARK_WITH_SODIUM=ON` plus `-DQUARK_SODIUM_INCLUDE_DIR=... -DQUARK_SODIUM_LIBRARY=...`
   (Windows static libsodium: also `-DQUARK_SODIUM_STATIC=ON`). Pinned campaign on Windows under MSVC ASan:

   ```bash
   cmake -S . -B build-fz-asan -G "Visual Studio 18 2026" -A x64 -DQUARK_SANITIZE=address -DQUARK_WITH_SODIUM=ON ...
   cmake --build build-fz-asan --config Release --target fuzz_decoders_test fuzz_pinned_oob_control
   build-fz-asan/tests/Release/fuzz_decoders_test --iters 300000   # run from a vcvars shell (ASan runtime DLL)
   ```

   **Control** (`fuzz_pinned_oob_control`, ASan builds only, `WILL_FAIL`): the same driver with
   `-DQUARK_FUZZ_PLANT_OOB`, which plants a one-byte overread in `control_msg1_parse_copy`. It must trap; a campaign
   that cannot find it is unproven. For libFuzzer: build a second tree with `-DCMAKE_CXX_FLAGS=-DQUARK_FUZZ_PLANT_OOB`
   and run `fuzz_decoders` on the corpus; it must report a heap-buffer-overflow in `control_msg1_parse_copy`.

2. **`fuzz_decoders`** (Clang libFuzzer + ASan/UBSan). Built only with `-DQUARK_BUILD_FUZZ=ON`;
   the CI Clang x86-64 leg runs a **60-second** campaign against the generated corpus on every
   push/PR.

   ```bash
   cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ \
     -DQUARK_BUILD_FUZZ=ON -DQUARK_BUILD_BENCH=OFF
   cmake --build build-fuzz --target fuzz_decoders fuzz_seed_corpus -j 2
   ./build-fuzz/tests/fuzz_seed_corpus build-fuzz/corpus
   ./build-fuzz/tests/fuzz_decoders -max_total_time=60 build-fuzz/corpus
   ```

   The corpus is generated from the same seed manifest (`seed_corpus()`), so a new decoder path
   adds both a deterministic seed and a libFuzzer seed.

## Triage

- A libFuzzer crash writes an artifact under `build-fuzz/corpus/` (the CI uploads the ctest logs;
  keep the artifact in the bug report). Reproduce with the exact artifact:
  `./build-fuzz/tests/fuzz_decoders <artifact>`.
- Confirm it is a real bug, not a deliberate control: the only intentionally failing target is the
  ASan OOB control (`serialize_fingerprint_mismatch_oob`, `WILL_FAIL`).
- Add the minimized artifact (or a hand-written equivalent) to the seed manifest so the regression
  is exercised forever, fix the decoder, and rerun the deterministic 2M campaign and the CI fuzz.
- A crash is a decoder bug, never a "flaky test": the inputs are deterministic.

## Current evidence

- Issue #50 (pinned-key pre-auth parsers), 2026-11-01: deterministic driver 400,000 iterations clean (MSVC Release, sodium on);
  MSVC ASan 300,000 iterations clean; libFuzzer 60 s on Linux (clang 21, ASan+UBSan, 80 seeds): 85,274 runs clean. The planted
  control trapped under MSVC ASan between 50,000 and 100,000 iterations and under libFuzzer in under one second. The campaign
  found one real bug: `decode_instance_label` accepted a spare zero group (`"a"` as the empty id, `"aaa"` as a second spelling of
  `{0}`); fixed, regression test `lan_discovery_label_canonical_test`, regression seeds in the manifest.

- The prior 2,000,000-input ASan+UBSan campaign over `decode_tagged`, `decode_control`,
  `decode_tagless_checked` was clean; `fuzz_decoders_test` extends that to the durable/migration,
  schema-advert, frame-body/reassembly and random-WAL paths.
- Lab runs: `fuzz_decoders_test --iters 200000` is clean (28 seeds + 100 random WAL replays); a
  60-second Clang libFuzzer + ASan/UBSan campaign completed **2,577,246 runs** with no crash /
  no timeout artifact.
- The CI `Fuzz decoders (libFuzzer, 60s)` step is the continuous gate; the deterministic test is
  part of the normal 4-config × 2-arch matrix, so Windows and both architectures are covered too.
- The F17 mixed-schema pair is deliberately *semantically* ambiguous in tagless bytes (that is the
  vulnerability the gate prevents); the fuzz asserts memory safety only, while `schema_probe` and
  `security_handshake_test` prove the gate chooses tagged/rejects across a schema mismatch.
