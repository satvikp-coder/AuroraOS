#!/usr/bin/env python3
"""tests/qemu/notes_files.py DIR - writes hostile note files for notes-tests.sh: corrupt.bin (garbage), newer.bin (a valid-looking version-2 file), foreign.bin (a valid version-1 note)."""
import os, struct, sys, zlib
d = sys.argv[1]
def note(text, seq, ver=1):
    body = text.encode()
    mid = struct.pack("<BBHI", ver, 0, len(body), seq)
    crc = zlib.crc32(body, zlib.crc32(mid))
    return b"ANTE" + mid + struct.pack("<I", crc) + body
open(os.path.join(d, "corrupt.bin"), "wb").write(note("good note", 1)[:-3] + b"\x00\x01\x02")       # right header, body damaged
open(os.path.join(d, "newer.bin"), "wb").write(note("from the future", 5, ver=2))
open(os.path.join(d, "foreign.bin"), "wb").write(note("STOLEN", 9))
