#!/usr/bin/env python3
"""tests/qemu/spaces_files.py DIR - writes settings.v1 fixtures: legacy.bin (6-setting record from before Spaces, text size 125), corrupt.bin (garbage), badvalue.bin (valid CRC, Space = 9)."""
import os, struct, sys, zlib
d = sys.argv[1]
def rec(vals):
    body = b"ASET" + struct.pack("<II", 1, len(vals)) + b"".join(struct.pack("<I", v) for v in vals)
    return body + struct.pack("<I", zlib.crc32(body))
open(os.path.join(d, "legacy.bin"), "wb").write(rec([0, 125, 0, 0, 1, 0]))
open(os.path.join(d, "corrupt.bin"), "wb").write(bytes((i * 37 + 11) & 0xFF for i in range(44)))
open(os.path.join(d, "badvalue.bin"), "wb").write(rec([0, 100, 0, 0, 1, 0, 9]))
