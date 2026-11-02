#!/usr/bin/env bash
# Build and install mbedTLS from its release tarball, hash-verified against ci/pins.env (issue #71).
# Ubuntu 24.04 ships mbedTLS 2.28, below the 3.6.2 floor (MBEDTLS_MIN) this repo needs (TLS 1.3), so the
# opt-in QUARK_WITH_MBEDTLS legs build the pinned release instead: a different hash, a different version
# or a failed extraction stops the job (fail closed). Installs static libraries under $1 (default /opt/mbedtls).
#   bash ci/install-mbedtls.sh [prefix]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=pins.env
source "$root/ci/pins.env"
prefix="${1:-/opt/mbedtls}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

tarball="$work/mbedtls-${MBEDTLS_SRC_VERSION}.tar.bz2"
curl -fsSL -o "$tarball" \
    "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_SRC_VERSION}/mbedtls-${MBEDTLS_SRC_VERSION}.tar.bz2"
got="$(sha256sum "$tarball" | awk '{print $1}')"
if [[ "$got" != "$MBEDTLS_SRC_SHA256" ]]; then
    echo "install-mbedtls: SHA-256 mismatch for mbedtls-${MBEDTLS_SRC_VERSION}: got $got, pinned $MBEDTLS_SRC_SHA256" >&2
    exit 1
fi
# python3 (already in the image) extracts the bz2 tarball: no extra apt package to lock.
python3 -c 'import sys, tarfile; tarfile.open(sys.argv[1]).extractall(sys.argv[2], filter="data")' "$tarball" "$work"
src="$work/mbedtls-${MBEDTLS_SRC_VERSION}"
cmake -S "$src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="g++-${GCC_MAJOR}" \
    -DCMAKE_C_COMPILER="gcc-${GCC_MAJOR}" -DCMAKE_INSTALL_PREFIX="$prefix" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "$work/build" -j 2
cmake --install "$work/build"
echo "install-mbedtls: ${MBEDTLS_SRC_VERSION} installed in $prefix"
