# Supply chain (P4)

Quark's build is a chain of third-party inputs: GitHub Actions, runner images/toolchains, the LLVM
installer, the vcpkg baseline, and mbedTLS. This document records what is pinned, how a change is
detected, how to bump it, and what is deliberately left to the runner platform.

## Policy

1. **Every Action is pinned to a full 40-hex commit**, never a tag or branch.
2. **Runner labels are release-pinned** (`ubuntu-24.04`, `ubuntu-24.04-arm`, `windows-2022`);
   `*-latest` is forbidden.
3. **The LLVM installer is hash-pinned** in `ci/pins.env` and verified before `sudo`; the script is
   executed by path, never piped to a shell.
4. **vcpkg is baseline-pinned** and **mbedTLS has a minimum version** enforced at configure/compile
   time; the pin file must agree with `vcpkg.json`.
5. **Only allowlisted apt packages** may be installed (`ci/apt-packages.txt`); a new package is a
   deliberate supply-chain change. Exact resolved versions are captured in the SBOM.
6. **Every build produces a CycloneDX SBOM**, covered by a SHA-256 manifest and a GitHub-signed
   build-provenance attestation.
7. **No remote script may be piped into `sudo`.** Download, verify, then execute.

## Pinned inputs

| Input | Where | Enforcement |
|---|---|---|
| GitHub Actions commits | `.github/workflows/*.yml` | `check_pins.sh`: `uses:@<40-hex>` only |
| Ubuntu x86-64 / arm64 image | `ci/pins.env`, `runs-on` | release label only; no `-latest` |
| Windows image | `ci/pins.env`, `runs-on` | release label only; exact MSVC in SBOM |
| LLVM installer | `LLVM_SH_SHA256` | SHA-256 verified pre-execution |
| GCC / Clang / LLVM majors | `GCC_MAJOR` / `CLANG_MAJOR` / `LLVM_MAJOR` | installer + CI matrix must agree |
| vcpkg baseline | `VCPKG_BASELINE` | must equal `vcpkg.json` `builtin-baseline` |
| mbedTLS floor | `MBEDTLS_MIN` | `vcpkg.json` `version>=` must be ≥ the floor |
| apt package set | `ci/apt-packages.txt` | installer packages ⊆ allowlist |

The single toolchain installer is `ci/install-deps.sh`. The single pin source is `ci/pins.env`.

## What the audit enforces

`ci/check_pins.sh` fails the build when:

- any Action reference is not a 40-hex commit (local `./` actions excepted);
- any `runs-on` uses a `*-latest` label, or a release label is missing;
- the LLVM hash is malformed, the installer does not verify it, or the hash is not sourced from
  `ci/pins.env`;
- the compiler matrix/installer disagrees with the pinned majors;
- `vcpkg.json`'s baseline differs from `VCPKG_BASELINE`, or mbedTLS is below `MBEDTLS_MIN`;
- the installer installs a package not in `ci/apt-packages.txt`, or an allowlisted package is no
  longer installed.

`bash ci/check_pins.sh --self-test` first proves the audit's teeth with two negative controls: a
mutable `actions/checkout@v4` reference and a malformed LLVM hash must both be rejected. The CI
`supply-chain` job runs the self-test and the real audit; every test/perf job has
`needs: supply-chain`, so a red pin audit blocks all builds.

## SBOM and provenance

`ci/gen_sbom.sh` writes CycloneDX 1.5 JSON containing:

- the project component (commit), mbedTLS, vcpkg, GCC/Clang/CMake/Ninja/GDB versions;
- every pinned Action and commit;
- every installed allowlisted apt package and its exact resolved version;
- SHA-256 hashes of `vcpkg.json`, `ci/pins.env`, `ci/install-deps.sh`, `ci/check_pins.sh`,
  `CMakeLists.txt`;
- runner image/OS/arch and the pin values.

The `supply-chain` CI job uploads `sbom.json` + `sbom.sha256` as an immutable artifact and attests
`sbom.json` with `actions/attest-build-provenance` (GitHub OIDC), so a consumer can verify which
commit/toolchain produced a given SBOM. The Action itself is commit-pinned. Attestation is skipped
for fork pull requests (no OIDC `id-token` permission); forks still get the SBOM + checksums.

## Bumping procedure

1. Change **only** the pin(s) in `ci/pins.env` and/or `vcpkg.json`, plus the matching
   `ci/apt-packages.txt` entry and workflow matrix if a compiler major changes.
2. Run `bash ci/check_pins.sh --self-test && bash ci/check_pins.sh` locally.
3. Run the full CI matrix; the SBOM diff is the review artifact (commit/toolchain/apt versions).
4. Merge the pin-only change separately from code changes; never "fix" a red audit by removing the
   pin. The LLVM script hash comes from `sha256sum` of the exact downloaded bytes; an Action SHA
   comes from `git ls-remote <repo> refs/tags/<tag>^{}` (the peeled commit, not the tag object).

## Residuals (explicit)

- **Apt and MSVC patch versions are runner-managed.** The runner images are release-pinned by
  label, but GitHub refreshes them with security updates. Exact versions are captured in the SBOM;
  a refresh shows up as an SBOM diff. A byte-identical rebuild therefore requires the same runner
  image, not merely the same commit. A container-image digest or exact package lock is the next
  step if bit-reproducibility is required.
- **The SBOM describes inputs, not a hermetic build.** Compiler output can vary with the host
  image; the source + pin manifest + SBOM identify the inputs, and the provenance attestation ties
  the SBOM to the CI run.
- **No artifact signing beyond the SBOM attestation.** Release binaries/libraries are not yet
  signed; that is a separate distribution task.
