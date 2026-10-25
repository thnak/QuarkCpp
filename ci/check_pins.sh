#!/usr/bin/env bash
# Fail-closed supply-chain audit. A changed upstream input is REJECTED until it is bumped
# deliberately in ci/pins.env (and the matching workflow/install script), then the full CI reruns.
#
#   bash ci/check_pins.sh              # audit this checkout
#   bash ci/check_pins.sh --self-test  # prove the audit rejects a mutable action and a bad hash
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

    # 3. The LLVM installer is hash-checked, and the hash lives in exactly one place (pins.env).
    grep -Fq 'ci/install-deps.sh' "$root/.github/workflows/ci.yml" ||
        fail "workflow does not use ci/install-deps.sh"
    grep -Fq 'LLVM_SH_SHA256' "$root/ci/install-deps.sh" ||
        fail "ci/install-deps.sh does not verify LLVM_SH_SHA256"
    grep -Fq 'sha256sum -c -' "$root/ci/install-deps.sh" ||
        fail "ci/install-deps.sh has no sha256sum verification"
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

    # 5. Only allowlisted apt packages may be installed. Versions are resolved from the pinned
    #    runner image and captured in the SBOM; adding a package needs a deliberate allowlist edit.
    local expanded installed pkg
    expanded="$(sed -e "s/\${GCC_MAJOR}/$GCC_MAJOR/g" -e "s/\${CLANG_MAJOR}/$CLANG_MAJOR/g" \
        "$root/ci/install-deps.sh")"
    installed="$(printf '%s\n' "$expanded" |
        awk '/apt-get install/{grab=1} grab{print} grab && $0 !~ /\\$/{grab=0}' |
        grep -oE 'g\+\+-[0-9]+|clang-[0-9]+|libc\+\+-[0-9]+-dev|libc\+\+abi-[0-9]+-dev|cmake|ninja-build|gdb' |
        sort -u)"
    for pkg in $installed; do
        grep -qxF "$pkg" "$root/ci/apt-packages.txt" ||
            fail "unlisted apt package '$pkg' (add it to ci/apt-packages.txt deliberately)"
    done
    while IFS= read -r pkg; do
        [[ -z "$pkg" || "$pkg" == \#* ]] && continue
        printf '%s\n' "$installed" | grep -qxF "$pkg" ||
            fail "allowlisted apt package '$pkg' is not installed by ci/install-deps.sh"
    done < "$root/ci/apt-packages.txt"

    echo "pin audit: OK (actions SHA-pinned; release runners; LLVM SHA-256; vcpkg/mbedTLS; apt allowlist)"
}

self_test() {
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN
    mkdir -p "$tmp/ci" "$tmp/.github/workflows"
    cp "$script_root/ci/pins.env" "$script_root/ci/apt-packages.txt" \
       "$script_root/ci/install-deps.sh" "$tmp/ci/"
    cp "$script_root/vcpkg.json" "$tmp/"
    cp "$script_root/.github/workflows/ci.yml" "$tmp/.github/workflows/"

    # Negative control 1: a mutable action tag must be rejected.
    sed -i -E 's#(uses:[[:space:]]*actions/checkout@)[0-9a-f]{40}#\1v4#' \
        "$tmp/.github/workflows/ci.yml"
    if (audit_root "$tmp") >/dev/null 2>&1; then
        fail "self-test: a mutable action tag was accepted"
    fi

    # Negative control 2: a changed LLVM installer hash must be rejected.
    cp "$script_root/.github/workflows/ci.yml" "$tmp/.github/workflows/"
    sed -i -E 's/^LLVM_SH_SHA256=.*/LLVM_SH_SHA256=deadbeef/' "$tmp/ci/pins.env"
    if (audit_root "$tmp") >/dev/null 2>&1; then
        fail "self-test: a malformed LLVM hash was accepted"
    fi

    echo "pin audit self-test: OK (mutable action + bad LLVM hash rejected)"
}

if [[ "${1:-}" == "--self-test" ]]; then
    self_test
fi
audit_root "$script_root"
