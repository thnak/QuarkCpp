#!/usr/bin/env python3
"""Issue #51 (assessment M6): differential test of the Noise IK glue against an independent implementation.

Generates N random transcripts (keys, ephemerals, prologue, payloads of random length), runs the C++ tool
(noise_ik_differential_tool) and the Python `noiseprotocol` package (Noise_IK_25519_ChaChaPoly_BLAKE2b) over the SAME inputs, and
compares every handshake message, both handshake hashes, all four split keys and the first transport ciphertext in each direction
byte for byte. This is a test tool, not a dependency of the library.

  noise_ik_differential.py --tool PATH [--n 10000] [--seed S] [--expect-mismatch]

Exit 0 when all N transcripts are identical. With --expect-mismatch (the CONTROL: a tool built from a noise.hpp with one changed line
in MixHash / MixKey) exit 0 only when the mismatch IS detected. Implements ADR-068 claim V1; 020-Security.
"""
import argparse
import os
import random
import subprocess
import sys
import warnings

# noiseprotocol warns that a preset ephemeral key is test-only; presetting it is exactly what a differential test must do.
warnings.filterwarnings("ignore", message="One of ephemeral keypairs is already set")

try:
    from noise.connection import Keypair, NoiseConnection
except ImportError:  # pragma: no cover
    print("noise_ik_differential: the 'noiseprotocol' package is not installed (pip install noiseprotocol)", file=sys.stderr)
    sys.exit(77)

PROTOCOL = b"Noise_IK_25519_ChaChaPoly_BLAKE2b"


def hx(b: bytes) -> str:
    return b.hex() if b else "-"


def reference(case):
    prologue, si, ei, sr, er, p1, p2, t1, t2 = case
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
    from cryptography.hazmat.primitives import serialization

    def pub(sk):
        return X25519PrivateKey.from_private_bytes(sk).public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)

    ini = NoiseConnection.from_name(PROTOCOL)
    ini.set_as_initiator()
    ini.set_prologue(prologue)
    ini.set_keypair_from_private_bytes(Keypair.STATIC, si)
    ini.set_keypair_from_public_bytes(Keypair.REMOTE_STATIC, pub(sr))
    ini.set_keypair_from_private_bytes(Keypair.EPHEMERAL, ei)
    rsp = NoiseConnection.from_name(PROTOCOL)
    rsp.set_as_responder()
    rsp.set_prologue(prologue)
    rsp.set_keypair_from_private_bytes(Keypair.STATIC, sr)
    rsp.set_keypair_from_private_bytes(Keypair.EPHEMERAL, er)
    ini.start_handshake()
    rsp.start_handshake()
    msg1 = ini.write_message(p1)
    rsp.read_message(msg1)
    msg2 = rsp.write_message(p2)
    ini.read_message(msg2)
    assert ini.handshake_finished and rsp.handshake_finished
    ip, rp = ini.noise_protocol, rsp.noise_protocol
    c1 = ini.encrypt(t1)
    c2 = rsp.encrypt(t2)
    return " ".join([hx(msg1), hx(msg2), hx(ip.handshake_hash), hx(rp.handshake_hash),
                     hx(ip.cipher_state_encrypt.k), hx(ip.cipher_state_decrypt.k),
                     hx(rp.cipher_state_encrypt.k), hx(rp.cipher_state_decrypt.k), hx(c1), hx(c2)])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--n", type=int, default=10000)
    ap.add_argument("--seed", type=int, default=51)
    ap.add_argument("--expect-mismatch", action="store_true")
    args = ap.parse_args()

    rnd = random.Random(args.seed)
    cases = []
    for _ in range(args.n):
        cases.append((rnd.randbytes(rnd.randint(0, 40)), rnd.randbytes(32), rnd.randbytes(32), rnd.randbytes(32),
                      rnd.randbytes(32), rnd.randbytes(rnd.randint(0, 96)), rnd.randbytes(rnd.randint(0, 96)),
                      rnd.randbytes(rnd.randint(0, 64)), rnd.randbytes(rnd.randint(0, 64))))
    stdin = "\n".join(" ".join(hx(f) for f in c) for c in cases) + "\n"
    out = subprocess.run([os.path.abspath(args.tool)], input=stdin, capture_output=True, text=True, check=False)
    if out.returncode != 0:
        print(f"tool failed ({out.returncode}): {out.stderr.strip()}", file=sys.stderr)
        return 1
    got = out.stdout.splitlines()
    if len(got) != len(cases):
        print(f"tool returned {len(got)} lines for {len(cases)} cases", file=sys.stderr)
        return 1
    same = 0
    first_bad = None
    for i, (c, line) in enumerate(zip(cases, got)):
        if reference(c) == line:
            same += 1
        elif first_bad is None:
            first_bad = i
    print(f"noise_ik_differential: {same}/{len(cases)} transcripts identical (seed {args.seed})")
    if args.expect_mismatch:
        if same == len(cases):
            print("CONTROL NOT DETECTED: the mutated engine matched the reference on every transcript", file=sys.stderr)
            return 1
        print(f"CONTROL detected: first difference at transcript {first_bad}")
        return 0
    if same != len(cases):
        print(f"MISMATCH at transcript {first_bad}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
