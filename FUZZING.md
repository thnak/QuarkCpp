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

## Two drivers, one target set

1. **`fuzz_decoders_test`** (in the default `ctest`, every platform/compiler). It seeds every path
   from the shared corpus, runs a deterministic random campaign, and does 100 random-byte
   `FileStore` replays.

   ```bash
   ./build/tests/fuzz_decoders_test                 # 50k default
   ./build/tests/fuzz_decoders_test --iters 2000000 # long campaign (ASan/UBSan build recommended)
   ```

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
