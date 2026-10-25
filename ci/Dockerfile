# Digest-addressed Linux CI image. The base is pinned by manifest-list digest (the OS libraries/
# kernel are then immutable); the exact apt toolchain comes from ci/apt-packages.txt. Build with
# .github/workflows/ci-image.yml (multi-arch), then pin the pushed image digest as CI_IMAGE in
# ci/pins.env and use it as the `container:` image for the Linux jobs.
#
# syntax=docker/dockerfile:1
FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac

ENV DEBIAN_FRONTEND=noninteractive

# curl/ca-certificates are needed to fetch and hash-verify llvm.sh; install-deps then installs the
# exact locked toolchain (including git/curl/python3/gh/wget/gnupg/lsb-release) and the exact
# clang/libc++ from apt.llvm.org.
COPY ci/apt-packages.txt ci/pins.env ci/install-deps.sh /tmp/ci/
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl ca-certificates \
 && bash /tmp/ci/install-deps.sh --with-clang \
 && rm -rf /var/lib/apt/lists/* /tmp/ci
