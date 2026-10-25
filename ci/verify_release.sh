#!/usr/bin/env bash
# Verify a keyless Sigstore signature produced by .github/workflows/release.yml.
#
#   cosign verify-blob --bundle <artifact>.sigstore.json \
#     --certificate-identity-regexp 'https://github.com/<repo>/.github/workflows/release.yml@refs/tags/<tag>' \
#     --certificate-oidc-issuer https://token.actions.githubusercontent.com <artifact>
#
# The script pins the expected workflow identity and issuer so a signature from any other workflow
# (or any other repository) is rejected.
set -euo pipefail

[[ $# -eq 2 ]] || {
    echo "usage: $0 <artifact> <artifact>.sigstore.json" >&2
    exit 2
}

artifact="$1"
bundle="$2"
repo="${GITHUB_REPOSITORY:-thnak/QuarkCpp}"
tag="${GITHUB_REF_NAME:-}"

if [[ -n "$tag" ]]; then
    identity="https://github.com/${repo}/.github/workflows/release.yml@refs/tags/${tag}"
    # Escape regex metacharacters in the tag (e.g. the dots in v1.0.0).
    identity_re="$(printf '%s' "$identity" | sed 's/[][\\.^$*+?(){}|]/\\&/g')"
else
    identity_re="https://github.com/${repo}/.github/workflows/release.yml@refs/tags/.*"
fi

cosign verify-blob \
    --bundle "$bundle" \
    --certificate-identity-regexp "$identity_re" \
    --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
    "$artifact"

echo "verified: $artifact (identity $identity_re)"
