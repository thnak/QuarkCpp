#!/usr/bin/env bash
# Install the Python reference for the Noise IK differential test (issue #51) from hash-pinned wheels (ci/pins.env NOISE_REF_WHEELS).
# The CI image has python3 but no pip, so each wheel is downloaded, its SHA-256 verified (a mismatch stops the job: fail closed) and
# unzipped into $1 (default /opt/noise-ref); run the test with PYTHONPATH=$1. A TEST tool only.
#   bash ci/install-noise-reference.sh [dir]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=pins.env
source "$root/ci/pins.env"
dir="${1:-/opt/noise-ref}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$dir"
for entry in $NOISE_REF_WHEELS; do
    url="${entry%%=*}"
    want="${entry##*=}"
    file="$work/${url##*/}"
    curl -fsSL -o "$file" "$url"
    got="$(sha256sum "$file" | awk '{print $1}')"
    if [[ "$got" != "$want" ]]; then
        echo "install-noise-reference: SHA-256 mismatch for ${url##*/}: got $got, pinned $want" >&2
        exit 1
    fi
    python3 -m zipfile -e "$file" "$dir"
done
PYTHONPATH="$dir" python3 -c 'import noise.connection, cryptography; print("install-noise-reference: ok")'
