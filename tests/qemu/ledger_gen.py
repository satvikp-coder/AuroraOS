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

def rec_bytes(seq, pid, principal, kind, state=1, arg=0, uptime=0):
    return struct.pack("<IIIBBBBBBBBiII", seq, pid, principal, kind, 0, 0, 0, 0, state, 0, 0, arg, uptime, 0)

def chain_of(prev, rec32):
    h = 1469598103934665603
    for x in struct.pack("<Q", prev) + rec32: h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

def trail_image(n):
    """a Trail image with n records, kinds 12,13,14,12,... : returns (image, head chain)"""
    prev = 0; body = b""
    for i in range(n):
        r = rec_bytes(i + 1, 0, 1000 + i, 12 + i % 3, uptime=i * 10)
        c = chain_of(prev, r); body += r + struct.pack("<Q", c); prev = c
    head = b"TRL1" + bytes([1, n, 0, 0]) + struct.pack("<IQI", n + 1, 0, 0) + body
    return head + struct.pack("<I", fnv(head)), prev

def tcontainer(n, version=1):
    img, head = trail_image(n)
    c = b"LGTR" + struct.pack("<III", version, n, len(img)) + img
    c = c + bytes(1996 - len(c))
    return c + struct.pack("<I", zlib.crc32(c) & 0xFFFFFFFF), head

def anchor_full(pp, tn, th):
    a = b"ANCH" + struct.pack("<IIIQI", 1, pp, tn, th, 0)
    return a + struct.pack("<I", zlib.crc32(a) & 0xFFFFFFFF)

def main():
    out = sys.argv[1]; os.makedirs(out, exist_ok=True)
    G = [(1001, 3), (1002, 1)]                       # (principal, permission): files-write for 1001, navigate for 1002
    files = {
        "p_good2.bin": container(G, 2), "p_good3.bin": container(G, 3),
        "a1.bin": anchor(1), "a2.bin": anchor(2), "a3.bin": anchor(3), "a_newer.bin": anchor(2, version=9),
        "p_newer.bin": container(G, 2, version=2), "garbage.bin": bytes((i * 37 + 11) & 0xFF for i in range(560)),
    }
    t3, h3 = tcontainer(3); t2, h2 = tcontainer(2)
    files.update({"t_good3.bin": t3, "t_newer.bin": tcontainer(3, version=2)[0], "at_ok.bin": anchor_full(2, 3, h3), "at_behind.bin": anchor_full(2, 5, h3),
                  "at_ahead1.bin": anchor_full(2, 2, h2), "at_badhead.bin": anchor_full(2, 3, h3 ^ 1), "at_ahead2.bin": anchor_full(2, 1, 0)})
    tf = bytearray(t3); tf[100] ^= 1; files["t_flipped.bin"] = bytes(tf)
    flipped = bytearray(container(G, 2)); flipped[100] ^= 1; files["p_flipped.bin"] = bytes(flipped)
    for name, data in files.items():
        assert (len(data) == 2000) if name.startswith("t_") else (len(data) == 560) if name.startswith("p_") or name == "garbage.bin" else len(data) == 32, name
        open(os.path.join(out, name), "wb").write(data)

if __name__ == "__main__":
    main()
