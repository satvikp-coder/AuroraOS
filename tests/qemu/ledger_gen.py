#!/usr/bin/env python3
"""tests/qemu/ledger_gen.py - writes hand-built ledger files for tests/qemu/ledger-tests.sh (the host builds the disk with mkafs, so the guest meets damaged,
newer and inconsistent ledgers it did not write itself). Layout = kernel/include/ledger.h: passport.v1 is 560 bytes ("LGPP", version, commit, image length,
Passport image, zero pad, CRC-32), anchor.v1 is 32 bytes ("ANCH", version, commit, reserved Trail fields, CRC-32). The Passport image is "PPRT" ver count
reserved seq | count x {principal, 0, perm, pad} | FNV-1a 32. Usage: ledger_gen.py <outdir>"""
import os, struct, sys, zlib

def fnv(b):
    h = 2166136261
    for x in b: h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h

def image(grants, seq=7):
    body = b"PPRT" + bytes([1, len(grants), 0, 0]) + struct.pack("<I", seq)
    for pr, perm in grants: body += struct.pack("<IIIBBBB", pr, 0, 0, perm, 0, 0, 0)
    return body + struct.pack("<I", fnv(body))

def container(grants, commit, version=1):
    img = image(grants)
    c = b"LGPP" + struct.pack("<III", version, commit, len(img)) + img
    c = c + b"\0" * (556 - len(c))
    return c + struct.pack("<I", zlib.crc32(c) & 0xFFFFFFFF)

def anchor(commit, version=1):
    a = b"ANCH" + struct.pack("<II", version, commit) + b"\0" * 16
    return a + struct.pack("<I", zlib.crc32(a) & 0xFFFFFFFF)

def main():
    out = sys.argv[1]; os.makedirs(out, exist_ok=True)
    G = [(1001, 3), (1002, 1)]                       # (principal, permission): files-write for 1001, navigate for 1002
    files = {
        "p_good2.bin": container(G, 2), "p_good3.bin": container(G, 3),
        "a1.bin": anchor(1), "a2.bin": anchor(2), "a3.bin": anchor(3), "a_newer.bin": anchor(2, version=9),
        "p_newer.bin": container(G, 2, version=2), "garbage.bin": bytes((i * 37 + 11) & 0xFF for i in range(560)),
    }
    flipped = bytearray(container(G, 2)); flipped[100] ^= 1; files["p_flipped.bin"] = bytes(flipped)
    for name, data in files.items():
        assert (len(data) == 560) if name.startswith("p_") or name == "garbage.bin" else len(data) == 32, name
        open(os.path.join(out, name), "wb").write(data)

if __name__ == "__main__":
    main()
