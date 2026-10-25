#!/usr/bin/env bash
# Fail-closed supply-chain audit. A changed upstream input is REJECTED until it is bumped
# deliberately in ci/pins.env / ci/apt-packages.txt (and the matching workflow/install script),
# then the full CI reruns.
#
#   bash ci/check_pins.sh              # audit this checkout
#   bash ci/check_pins.sh --self-test  # prove the audit rejects mutable/bad/wildcard pins
#   QUARK_VERIFY_INSTALLED=1 bash ci/check_pins.sh   # also require dpkg versions == the exact lock
set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail() {
    echo "PIN AUDIT FAIL: $*" >&2
    exit 1
}

audit_root() {
    local root="$1"
    # shellcheck source=ci/pins.env
    source "$root/ci/pins.env"

    [[ "$LLVM_SH_SHA256" =~ ^[0-9a-f]{64}$ ]] ||
        fail "ci/pins.env: LLVM_SH_SHA256 must be 64 lowercase hex (got '$LLVM_SH_SHA256')"
    [[ "$VCPKG_BASELINE" =~ ^[0-9a-f]{40}$ ]] ||
        fail "ci/pins.env: VCPKG_BASELINE must be a 40-hex commit"
    [[ "$GCC_MAJOR" =~ ^[0-9]+$ && "$CLANG_MAJOR" =~ ^[0-9]+$ && "$LLVM_MAJOR" =~ ^[0-9]+$ ]] ||
        fail "ci/pins.env: GCC_MAJOR/CLANG_MAJOR/LLVM_MAJOR must be numeric"
    [[ "$MSVC_VERSION" =~ ^[0-9]+(\.[0-9]+){2}$ ]] ||
        fail "ci/pins.env: MSVC_VERSION must be an exact 3-part toolset version (got '$MSVC_VERSION')"
    [[ "$COSIGN_VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
        fail "ci/pins.env: COSIGN_VERSION must be vMAJOR.MINOR.PATCH (got '$COSIGN_VERSION')"

    # 1. Every `uses:` is a 40-hex commit, never a tag/branch. Local composite actions (./...) are ok.
    local ref
    while IFS= read -r ref; do
        ref="${ref%%#*}"
        ref="$(echo "$ref" | xargs)"
        [[ -z "$ref" ]] && continue
        [[ "$ref" == ./* ]] && continue
        [[ "$ref" =~ @[0-9a-f]{40}$ ]] ||
            fail "unpinned GitHub Action: '$ref' (pin the full 40-hex commit SHA)"
    done < <(grep -RhoE '^[[:space:]]*(-[[:space:]]+)?uses:[[:space:]]*[^[:space:]]+' \
        "$root/.github/workflows" | sed -E 's/^[[:space:]]*(-[[:space:]]+)?uses:[[:space:]]*//')

    # 2. Release-pinned runner labels only (`*-latest` is a moving tag), and they must match pins.
    if grep -RInE '^[[:space:]]*runs-on:.*-latest' "$root/.github/workflows" >/dev/null; then
        fail "a workflow uses a '*-latest' runner label (pin a release image instead)"
    fi
    grep -RFq "$UBUNTU_RUNNER" "$root/.github/workflows" || fail "workflow is missing $UBUNTU_RUNNER"
    grep -RFq "$UBUNTU_ARM_RUNNER" "$root/.github/workflows" || fail "workflow is missing $UBUNTU_ARM_RUNNER"
    grep -RFq "$WINDOWS_RUNNER" "$root/.github/workflows" || fail "workflow is missing $WINDOWS_RUNNER"

    # 3. The LLVM installer is hash-checked, and its unpinned package set is neutralized so only the
    #    exact apt lock below is installed.
    grep -Fq 'ci/install-deps.sh' "$root/.github/workflows/ci.yml" ||
        fail "workflow does not use ci/install-deps.sh"
    grep -Fq 'LLVM_SH_SHA256' "$root/ci/install-deps.sh" ||
        fail "ci/install-deps.sh does not verify LLVM_SH_SHA256"
    grep -Fq 'sha256sum -c -' "$root/ci/install-deps.sh" ||
        fail "ci/install-deps.sh has no sha256sum verification"
    grep -Fq 's#^apt-get install -y \$PKG$#:#' "$root/ci/install-deps.sh" ||
        fail "ci/install-deps.sh does not neutralize llvm.sh's unpinned apt install"
    grep -Fq "g++-\${GCC_MAJOR}" "$root/ci/install-deps.sh" ||
        fail "ci/install-deps.sh does not install g++-\${GCC_MAJOR}"
    grep -Fq 'cxx: g++-'"$GCC_MAJOR" "$root/.github/workflows/ci.yml" ||
        fail "workflow compiler matrix does not match GCC_MAJOR=$GCC_MAJOR"
    grep -Fq 'cxx: clang++-'"$CLANG_MAJOR" "$root/.github/workflows/ci.yml" ||
        fail "workflow compiler matrix does not match CLANG_MAJOR=$CLANG_MAJOR"

    # 4. The vcpkg baseline and the mbedTLS floor are pinned and consistent.
    python3 - "$root/vcpkg.json" "$VCPKG_BASELINE" "$MBEDTLS_MIN" <<'PY'
import json, sys
path, want_baseline, want_mbedtls = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, encoding="utf-8") as f:
    d = json.load(f)
got = d.get("builtin-baseline", "")
if got != want_baseline:
    sys.exit(f"vcpkg.json baseline {got!r} != pinned {want_baseline!r}")
def ver(s):
    return tuple(int(x) for x in s.split("."))
dep = None
for x in d.get("dependencies", []):
    if (isinstance(x, str) and x == "mbedtls") or (isinstance(x, dict) and x.get("name") == "mbedtls"):
        dep = x
        break
if dep is None:
    sys.exit("vcpkg.json does not declare mbedtls")
floor = dep.get("version>=") if isinstance(dep, dict) else None
if floor is None:
    sys.exit("vcpkg.json mbedtls has no version>= floor")
if ver(floor) < ver(want_mbedtls):
    sys.exit(f"mbedtls {floor} is below the pinned floor {want_mbedtls}")
PY

    # 5. Exact apt lock. Every entry is `pkg=exact-version` (no wildcard); the known package set is
    #    fixed; the installer consumes the lock through its helper, never a literal package name.
    local line pkg ver
    grep -Fq 'ci/apt-packages.txt' "$root/ci/install-deps.sh" ||
        fail "ci/install-deps.sh does not consume ci/apt-packages.txt"
    while IFS= read -r line; do
        [[ -z "$line" || "$line" == \#* ]] && continue
        [[ "$line" =~ ^[a-z0-9][a-z0-9+.-]*=[A-Za-z0-9][A-Za-z0-9.+:~_-]*$ ]] ||
            fail "ci/apt-packages.txt: not pkg=exact-version: '$line'"
        pkg="${line%%=*}"
        ver="${line#*=}"
        [[ "$ver" == *"*"* ]] && fail "ci/apt-packages.txt: wildcard version for '$pkg'"
        case "$pkg" in
            "g++-${GCC_MAJOR}"|cmake|ninja-build|gdb|"clang-${CLANG_MAJOR}"|\
            "libc++-${CLANG_MAJOR}-dev"|"libc++abi-${CLANG_MAJOR}-dev") ;;
            *) fail "ci/apt-packages.txt: unlisted package '$pkg'" ;;
        esac
    done < "$root/ci/apt-packages.txt"
    for pkg in "g++-${GCC_MAJOR}" cmake ninja-build gdb "clang-${CLANG_MAJOR}" \
               "libc++-${CLANG_MAJOR}-dev" "libc++abi-${CLANG_MAJOR}-dev"; do
        grep -Fq "${pkg}=" "$root/ci/apt-packages.txt" ||
            fail "ci/apt-packages.txt is missing the locked package '$pkg'"
    done
    while IFS= read -r line; do
        [[ "$line" == *'"${pkgs[@]}"'* ]] ||
            fail "ci/install-deps.sh apt-get install bypasses the exact lock: $line"
    done < <(grep -E '^[[:space:]]*sudo apt-get install' "$root/ci/install-deps.sh")

    # In CI after installation, prove the running toolchain IS the lock (dpkg is Linux-only).
    if [[ "${QUARK_VERIFY_INSTALLED:-0}" == 1 ]] && command -v dpkg-query >/dev/null; then
        while IFS= read -r line; do
            [[ -z "$line" || "$line" == \#* ]] && continue
            pkg="${line%%=*}"
            ver="${line#*=}"
            local got
            got="$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null || true)"
            [[ "$got" == "$ver" ]] ||
                fail "installed $pkg=${got:-missing} != locked $ver"
        done < "$root/ci/apt-packages.txt"
    fi

    # 6. The Windows job selects the exact MSVC toolset pinned in ci/pins.env.
    grep -Fq 'MSVC_VERSION=$MSVC_VERSION' "$root/.github/workflows/ci.yml" ||
        fail "ci.yml does not export MSVC_VERSION from ci/pins.env"
    grep -Fq 'version=%MSVC_VERSION%' "$root/.github/workflows/ci.yml" ||
        fail "ci.yml does not pass -T version=%MSVC_VERSION% to CMake"

    # 7. Release artifacts are signed by the pinned keyless signer and verified before publishing.
    [[ -f "$root/.github/workflows/release.yml" ]] || fail "missing .github/workflows/release.yml"
    grep -Fq "cosign-release: $COSIGN_VERSION" "$root/.github/workflows/release.yml" ||
        fail "release.yml cosign-release does not match COSIGN_VERSION=$COSIGN_VERSION"
    grep -Fq 'bash ci/verify_release.sh' "$root/.github/workflows/release.yml" ||
        fail "release.yml does not verify the signatures before publishing"

    echo "pin audit: OK (actions SHA-pinned; release runners; LLVM SHA-256 + repo-only; vcpkg/mbedTLS; exact apt lock; MSVC pin; keyless release signing)"
}

self_test() {
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN
    mkdir -p "$tmp/ci" "$tmp/.github/workflows"
    cp "$script_root/ci/pins.env" "$script_root/ci/apt-packages.txt" \
       "$script_root/ci/install-deps.sh" "$tmp/ci/"
    cp "$script_root/vcpkg.json" "$tmp/"
    cp "$script_root/.github/workflows/ci.yml" "$script_root/.github/workflows/release.yml" \
       "$tmp/.github/workflows/"

    # Negative control 1: a mutable action tag must be rejected.
    sed -i -E 's#(uses:[[:space:]]*actions/checkout@)[0-9a-f]{40}#\1v4#' \
        "$tmp/.github/workflows/ci.yml"
    if (audit_root "$tmp") >/dev/null 2>&1; then
        fail "self-test: a mutable action tag was accepted"
    fi
    cp "$script_root/.github/workflows/ci.yml" "$tmp/.github/workflows/"

    # Negative control 2: a changed LLVM installer hash must be rejected.
    sed -i -E 's/^LLVM_SH_SHA256=.*/LLVM_SH_SHA256=deadbeef/' "$tmp/ci/pins.env"
    if (audit_root "$tmp") >/dev/null 2>&1; then
        fail "self-test: a malformed LLVM hash was accepted"
    fi
    cp "$script_root/ci/pins.env" "$tmp/ci/"

    # Negative control 3: an apt entry without an exact version must be rejected.
    sed -i -E 's/^cmake=.*/cmake/' "$tmp/ci/apt-packages.txt"
    if (audit_root "$tmp") >/dev/null 2>&1; then
        fail "self-test: an unpinned apt package was accepted"
    fi
    cp "$script_root/ci/apt-packages.txt" "$tmp/ci/"

    # Negative control 4: a moving/invalid MSVC toolset must be rejected.
    sed -i -E 's/^MSVC_VERSION=.*/MSVC_VERSION=latest/' "$tmp/ci/pins.env"
    if (audit_root "$tmp") >/dev/null 2>&1; then
        fail "self-test: an invalid MSVC_VERSION was accepted"
    fi
    cp "$script_root/ci/pins.env" "$tmp/ci/"

    # Negative control 5: an invalid cosign release pin must be rejected.
    sed -i -E 's/^COSIGN_VERSION=.*/COSIGN_VERSION=v0/' "$tmp/ci/pins.env"
    if (audit_root "$tmp") >/dev/null 2>&1; then
        fail "self-test: an invalid COSIGN_VERSION was accepted"
    fi

    echo "pin audit self-test: OK (mutable action, bad LLVM hash, unpinned apt, invalid MSVC/cosign rejected)"
}

if [[ "${1:-}" == "--self-test" ]]; then
    self_test
fi
audit_root "$script_root"
