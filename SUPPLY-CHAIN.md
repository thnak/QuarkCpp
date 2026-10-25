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
5. **Every apt package is installed at an exact version** (`ci/apt-packages.txt`); no names-only
   entries, wildcards, or unlisted packages. The LLVM installer is used only to add its repo/key —
   its own unpinned `apt-get install` is neutralized — and `clang`/`libc++` come from the lock.
6. **The Windows MSVC toolset is exact**, selected with `-T version=%MSVC_VERSION%` from
   `ci/pins.env`; the pin is recorded in the SBOM. A runner refresh that lacks it fails closed.
7. **Every build produces a CycloneDX SBOM**, covered by a SHA-256 manifest and a GitHub-signed
   build-provenance attestation.
8. **Release artifacts are keylessly signed** (Sigstore/cosign over GitHub OIDC) and the signatures
   are verified before publishing, with the signing Action + cosign release pinned.
9. **Linux jobs run in a digest-addressed image**: the Ubuntu base is pinned by manifest-list
   digest and the toolchain image by its image digest (`CI_IMAGE`). `ci/Dockerfile` builds from the
   pinned base with the exact apt lock; `.github/workflows/ci-image.yml` publishes multi-arch and
   prints the digest to bump. The image is pinned in `CI_IMAGE`; a toolchain change requires a new
   image build and pin.
10. **No remote script may be piped into `sudo`.** Download, verify, then execute.

## Pinned inputs

| Input | Where | Enforcement |
|---|---|---|
| GitHub Actions commits | `.github/workflows/*.yml` | `check_pins.sh`: `uses:@<40-hex>` only |
| Ubuntu x86-64 / arm64 image | `ci/pins.env`, `runs-on` | release label only; no `-latest` |
| Linux CI image | `CI_IMAGE` | digest-only; all Linux jobs use it |
| Ubuntu base | `UBUNTU_BASE_DIGEST` | `ci/Dockerfile` FROM digest; image build |
| Windows image | `ci/pins.env`, `runs-on` | release label only |
| Windows MSVC toolset | `MSVC_VERSION` | exact `cmake -T version=…`; pin in SBOM |
| LLVM installer | `LLVM_SH_SHA256` | SHA-256 verified pre-execution; repo-only patch |
| GCC / Clang / LLVM majors | `GCC_MAJOR` / `CLANG_MAJOR` / `LLVM_MAJOR` | installer + CI matrix must agree |
| apt package lock | `ci/apt-packages.txt` | exact `pkg=version`; installed version must match |
| vcpkg baseline | `VCPKG_BASELINE` | must equal `vcpkg.json` `builtin-baseline` |
| mbedTLS floor | `MBEDTLS_MIN` | `vcpkg.json` `version>=` must be ≥ the floor |
| Release signer | `COSIGN_VERSION` + pinned Action SHA | `release.yml` must match; verify before publish |

The single toolchain installer is `ci/install-deps.sh`. The single workflow pin source is
`ci/pins.env`; the exact apt versions live in `ci/apt-packages.txt`.

## What the audit enforces

`ci/check_pins.sh` fails the build when:

- any Action reference is not a 40-hex commit (local `./` actions excepted);
- any `runs-on` uses a `*-latest` label, or a release label is missing;
- the LLVM hash is malformed, the installer does not verify it, or the hash is not sourced from
  `ci/pins.env`;
- the compiler matrix/installer disagrees with the pinned majors;
- an apt entry is missing `=exact-version`, uses a wildcard, is unlisted, or the installer bypasses
  the lock; on a CI runner (`QUARK_VERIFY_INSTALLED=1`) the installed `dpkg` version must equal it;
- the LLVM helper's own package install is not neutralized, so it could bypass the exact lock;
- the Windows workflow does not export/use `MSVC_VERSION` through `-T version=%MSVC_VERSION%`;
- `CI_IMAGE` is not a digest-only ghcr image, is not used by every Linux job, or the Dockerfile
  base differs from `UBUNTU_BASE_DIGEST`;
- `vcpkg.json`'s baseline differs from `VCPKG_BASELINE`, or mbedTLS is below `MBEDTLS_MIN`;
- `release.yml` uses a cosign release other than `COSIGN_VERSION`, or does not verify signatures
  before publishing.

`bash ci/check_pins.sh --self-test` first proves the audit's teeth with seven negative controls: a
mutable `actions/checkout@v4` reference, a malformed LLVM hash, an apt entry without an exact
version, an invalid/moving `MSVC_VERSION`, an invalid `COSIGN_VERSION`, a mutable CI image, and a
non-digest base image must all be rejected.
The CI `supply-chain` job runs the self-test and the real audit with `QUARK_VERIFY_INSTALLED=1`;
every test/perf job has `needs: supply-chain`, so a red pin audit blocks all builds.

## SBOM and provenance

`ci/gen_sbom.sh` writes CycloneDX 1.5 JSON containing:

- the project component (commit), mbedTLS, vcpkg, GCC/Clang/CMake/Ninja/GDB versions;
- the exact **MSVC** toolset pin and the `windows-2022` selection;
- the digest-pinned Linux CI image (`CI_IMAGE`) and Ubuntu base manifest-list digest;
- every pinned Action and commit;
- every installed locked apt package and its exact resolved version;
- SHA-256 hashes of `vcpkg.json`, `ci/pins.env`, `ci/apt-packages.txt`, `ci/install-deps.sh`,
  `ci/check_pins.sh`, `ci/gen_sbom.sh`, `ci/verify_release.sh`, `ci/Dockerfile`, `CMakeLists.txt`,
  and the `ci.yml`/`ci-image.yml`/`release.yml` workflows;
- runner image/OS/arch and the pin values (including `COSIGN_VERSION`).

The `supply-chain` CI job uploads `sbom.json` + `sbom.sha256` as an immutable artifact and attests
`sbom.json` with `actions/attest-build-provenance` (GitHub OIDC), so a consumer can verify which
commit/toolchain produced a given SBOM. The Action itself is commit-pinned. Attestation is skipped
for fork pull requests (no OIDC `id-token` permission); forks still get the SBOM + checksums.

## Release signing

Pushing a `v*` tag runs `.github/workflows/release.yml`: it installs the exact toolchain, reruns
the pin audit, builds `libquark.a` + headers into a release archive, emits the SBOM, and signs
**both** the archive and the SBOM keylessly with Sigstore/cosign using GitHub OIDC. The bundle is
written next to each artifact (`<artifact>.sigstore.json`). The workflow calls
`ci/verify_release.sh` before publishing, which pins the expected certificate identity to this
repository's `release.yml` on a tag and the GitHub Actions OIDC issuer — a signature from any other
workflow or repository fails. It then attaches build-provenance attestations and publishes the
archive, SBOM, checksums, and both bundles.

Consumers verify with:

```bash
cosign verify-blob --bundle quark-vX.Y.Z-linux-x86_64.tar.gz.sigstore.json \
  --certificate-identity-regexp 'https://github.com/thnak/QuarkCpp/.github/workflows/release.yml@refs/tags/vX.Y.Z' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  quark-vX.Y.Z-linux-x86_64.tar.gz
```

or simply `bash ci/verify_release.sh <artifact> <bundle>` with `GITHUB_REPOSITORY`/`GITHUB_REF_NAME`
set. The `COSIGN_VERSION` pin and the `sigstore/cosign-installer` Action SHA are audited; a
different signer version fails the pin gate.

## Bumping procedure

1. Change **only** the pin(s) in `ci/pins.env`, `ci/apt-packages.txt`, and/or `vcpkg.json`, plus the
   matching workflow matrix if a compiler major changes. For apt, use
   `apt-cache policy <pkg>` (or the noble/LLVM `Packages` indexes) and record the exact version;
   for MSVC, read the toolset version from the `windows-2022` image manifest; for cosign, use the
   release tag + the Action commit SHA.
2. Run `bash ci/check_pins.sh --self-test && QUARK_VERIFY_INSTALLED=1 bash ci/check_pins.sh` on a
   runner with the exact packages installed.
3. Run the full CI matrix; the SBOM diff is the review artifact (commit/toolchain/apt versions).
   For a signed release, push a `v*` tag and verify the workflow's verify-before-publish step.
4. For the CI image: run `ci-image.yml` (or wait for its push build), copy the printed
   `ghcr.io/<owner>/quark-ci@sha256:<digest>` into `CI_IMAGE` and every `container.image` line,
   then rerun the pin audit. The placeholder digest must never be deployed.
5. Merge the pin-only change separately from code changes; never "fix" a red audit by removing the
   pin. The LLVM script hash comes from `sha256sum` of the exact downloaded bytes; an Action SHA
   comes from `git ls-remote <repo> refs/tags/<tag>^{}` (the peeled commit, not the tag object).

## Residuals (explicit)

- **The Linux base is now digest-pinned; the Windows base is still runner-managed.** Linux jobs run
  in `CI_IMAGE`, built by `ci-image.yml` from `ubuntu:24.04@sha256:<UBUNTU_BASE_DIGEST>` with the
  exact apt lock. A compiler/toolchain change requires a new image build and a new digest pin.
  The `windows-2022` base image itself still follows its release label; the exact MSVC toolset is
  selected from the pin, but a byte-identical Windows base is not possible on hosted runners.
- **The SBOM describes inputs, not a hermetic build.** Compiler output can vary with the host
  image; the source + pin manifest + SBOM identify the inputs, and the provenance attestation ties
  the SBOM to the CI run.
- **The release artifact is a Linux x86-64 archive + SBOM, not a cross-platform binary matrix.**
  The signing/verification mechanism is in place; producing Windows/arm64 release binaries is a
  separate distribution task.
