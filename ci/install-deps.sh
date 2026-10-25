#!/usr/bin/env bash
# Install the CI toolchain from the pinned inputs. No remote script is ever piped into sudo: the
# LLVM installer is downloaded, checked against the SHA-256 in `ci/pins.env`, then executed by path.
#
#   bash ci/install-deps.sh              # g++ + cmake/ninja/gdb
#   bash ci/install-deps.sh --with-clang # ... plus LLVM/clang (the hash-pinned installer)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=ci/pins.env
source "$root/ci/pins.env"

with_clang=0
if [[ "${1:-}" == "--with-clang" ]]; then
    with_clang=1
fi

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    "g++-${GCC_MAJOR}" \
    cmake \
    ninja-build \
    gdb

if ((with_clang)); then
    curl -fsSL -o /tmp/llvm.sh https://apt.llvm.org/llvm.sh
    echo "${LLVM_SH_SHA256}  /tmp/llvm.sh" | sha256sum -c -
    sudo bash /tmp/llvm.sh "${LLVM_MAJOR}"
    sudo apt-get install -y --no-install-recommends \
        "clang-${CLANG_MAJOR}" \
        "libc++-${CLANG_MAJOR}-dev" \
        "libc++abi-${CLANG_MAJOR}-dev"
fi
