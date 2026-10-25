#!/usr/bin/env bash
# Install the CI toolchain from the pinned inputs. No remote script is ever piped into sudo: the
# LLVM installer is downloaded, checked against the SHA-256 in `ci/pins.env`, then executed by path.
# The llvm.sh installer's own unpinned package install is neutralized (repo/key only); the compiler
# and libc++ packages are then installed at the EXACT versions in `ci/apt-packages.txt`.
#
#   bash ci/install-deps.sh              # g++ + cmake/ninja/gdb
#   bash ci/install-deps.sh --with-clang # ... plus LLVM/clang (hash-pinned installer + exact lock)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=ci/pins.env
source "$root/ci/pins.env"

with_clang=0
if [[ "${1:-}" == "--with-clang" ]]; then
    with_clang=1
fi

# The single exact-version source. Every package installed by this script must be listed here.
locked() {
    local pkg="$1" line
    line="$(awk -F= -v p="$pkg" '$1 == p { print; exit }' "$root/ci/apt-packages.txt")"
    if [[ -z "$line" ]]; then
        echo "apt lock: no exact version for '$pkg' in ci/apt-packages.txt" >&2
        exit 1
    fi
    printf '%s\n' "$line"
}

install_locked() {
    local pkgs=() p
    for p in "$@"; do pkgs+=("$(locked "$p")"); done
    # --allow-downgrades: the runner image may preinstall a newer version; the lock wins.
    sudo apt-get install -y --no-install-recommends --allow-downgrades "${pkgs[@]}"
}

sudo apt-get update
install_locked "g++-${GCC_MAJOR}" cmake ninja-build gdb

if ((with_clang)); then
    curl -fsSL -o /tmp/llvm.sh https://apt.llvm.org/llvm.sh
    echo "${LLVM_SH_SHA256}  /tmp/llvm.sh" | sha256sum -c -
    # Add the LLVM apt repo/key only. The script's final `apt-get install -y $PKG` would install
    # the repository's latest set (and packages outside our lock); neutralize just that line.
    sed -i 's#^apt-get install -y \$PKG$#:#' /tmp/llvm.sh
    sudo bash /tmp/llvm.sh "${LLVM_MAJOR}"
    install_locked "clang-${CLANG_MAJOR}" "libc++-${CLANG_MAJOR}-dev" "libc++abi-${CLANG_MAJOR}-dev"
fi
