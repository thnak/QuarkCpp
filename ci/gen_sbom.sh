#!/usr/bin/env bash
# Generate a CycloneDX 1.5 SBOM of the build inputs actually used by this checkout. The CI job
# uploads it as an immutable artifact and attests its provenance (GitHub OIDC); the companion
# `sbom.sha256` covers the SBOM + the pin manifests.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 - "$root" <<'PY'
import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys

root = sys.argv[1]


def run(*args):
    try:
        p = subprocess.run(args, cwd=root, capture_output=True, text=True, check=False)
        return p.stdout.strip() if p.returncode == 0 else ""
    except OSError:
        return ""


def sha256(path):
    h = hashlib.sha256()
    with open(os.path.join(root, path), "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def version_line(tool):
    out = run(tool, "--version")
    return out.splitlines()[0] if out else "not installed"


pins = {}
with open(os.path.join(root, "ci/pins.env"), encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        pins[k] = v

commit = os.environ.get("GITHUB_SHA") or run("git", "rev-parse", "HEAD") or "unknown"
server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
repo = os.environ.get("GITHUB_REPOSITORY") or "thnak/QuarkCpp"
run_id = os.environ.get("GITHUB_RUN_ID", "")
image_os = os.environ.get("ImageOS", "")
image_version = os.environ.get("ImageVersion", "")
image_version = image_version or run("cat", "/etc/quark-image-version")

apt_versions = {}
if shutil.which("dpkg-query"):
    with open(os.path.join(root, "ci/apt-packages.txt"), encoding="utf-8") as f:
        for line in f:
            pkg = line.strip()
            if not pkg or pkg.startswith("#"):
                continue
            v = run("dpkg-query", "-W", "-f=${Version}", pkg)
            if v:
                apt_versions[pkg] = v

# Every pinned GitHub Action (already enforced SHA-only by check_pins.sh).
actions = []
wf_dir = os.path.join(root, ".github/workflows")
for name in sorted(os.listdir(wf_dir)):
    if not name.endswith((".yml", ".yaml")):
        continue
    with open(os.path.join(wf_dir, name), encoding="utf-8") as f:
        for line in f:
            m = re.match(r"\s*-?\s*uses:\s*([^\s#]+)", line)
            if not m:
                continue
            ref = m.group(1)
            if ref.startswith("./"):
                continue
            action, _, sha = ref.partition("@")
            if (action, sha) not in actions:
                actions.append((action, sha))

components = [
    {
        "type": "library",
        "bom-ref": f"pkg:github/Mbed-TLS/mbedtls@{pins['MBEDTLS_MIN']}",
        "name": "mbedTLS",
        "version": pins["MBEDTLS_MIN"],
        "purl": f"pkg:github/Mbed-TLS/mbedtls@{pins['MBEDTLS_MIN']}",
        "properties": [
            {"name": "quark:vcpkgBaseline", "value": pins["VCPKG_BASELINE"]},
            {"name": "quark:floor", "value": pins["MBEDTLS_MIN"]},
        ],
    },
    {
        "type": "application",
        "bom-ref": f"pkg:github/microsoft/vcpkg@{pins['VCPKG_BASELINE']}",
        "name": "vcpkg",
        "version": pins["VCPKG_BASELINE"],
        "purl": f"pkg:github/microsoft/vcpkg@{pins['VCPKG_BASELINE']}",
    },
    {
        "type": "application",
        "bom-ref": "tool:gcc",
        "name": "gcc",
        "version": version_line("g++-" + pins["GCC_MAJOR"]),
    },
    {
        "type": "application",
        "bom-ref": "tool:clang",
        "name": "clang",
        "version": version_line("clang-" + pins["CLANG_MAJOR"]),
    },
    {"type": "application", "bom-ref": "tool:cmake", "name": "cmake", "version": version_line("cmake")},
    {"type": "application", "bom-ref": "tool:ninja", "name": "ninja", "version": version_line("ninja")},
    {"type": "application", "bom-ref": "tool:gdb", "name": "gdb", "version": version_line("gdb")},
]
for action, sha in actions:
    components.append(
        {
            "type": "application",
            "bom-ref": f"action:{action}@{sha}",
            "name": action,
            "version": sha,
            "properties": [{"name": "quark:actionCommit", "value": sha}],
        }
    )
for pkg, version in sorted(apt_versions.items()):
    components.append({"type": "library", "bom-ref": f"apt:{pkg}@{version}", "name": pkg, "version": version})
for path in ("vcpkg.json", "ci/pins.env", "ci/install-deps.sh", "ci/check_pins.sh", "CMakeLists.txt"):
    digest = sha256(path)
    components.append(
        {
            "type": "file",
            "bom-ref": f"file:{path}",
            "name": path,
            "version": digest,
            "hashes": [{"alg": "SHA-256", "content": digest}],
        }
    )

sbom = {
    "bomFormat": "CycloneDX",
    "specVersion": "1.5",
    "version": 1,
    "metadata": {
        "timestamp": datetime.datetime.now(datetime.timezone.utc)
        .isoformat()
        .replace("+00:00", "Z"),
        "tools": [{"vendor": "Quark", "name": "ci/gen_sbom.sh", "version": "1"}],
        "component": {
            "type": "application",
            "bom-ref": f"pkg:github/{repo}@{commit}",
            "name": "QuarkCpp",
            "version": commit[:12],
            "purl": f"pkg:github/{repo}@{commit}",
        },
        "properties": [
            {"name": "quark:git:commit", "value": commit},
            {"name": "quark:ci:run", "value": run_id},
            {"name": "quark:runner:image", "value": image_os},
            {"name": "quark:runner:version", "value": image_version or "unknown"},
            {"name": "quark:runner:os", "value": platform.platform()},
            {"name": "quark:runner:arch", "value": platform.machine()},
            {"name": "quark:pins:llvmSha256", "value": pins["LLVM_SH_SHA256"]},
            {"name": "quark:pins:llvmMajor", "value": pins["LLVM_MAJOR"]},
            {"name": "quark:pins:vcpkgBaseline", "value": pins["VCPKG_BASELINE"]},
            {"name": "quark:pins:mbedtlsMin", "value": pins["MBEDTLS_MIN"]},
            {"name": "quark:pins:ubuntuRunner", "value": pins["UBUNTU_RUNNER"]},
            {"name": "quark:pins:windowsRunner", "value": pins["WINDOWS_RUNNER"]},
        ],
        "externalReferences": [{"type": "vcs", "url": f"{server}/{repo}/commit/{commit}"}],
    },
    "components": components,
}
print(json.dumps(sbom, indent=2, sort_keys=True))
PY
