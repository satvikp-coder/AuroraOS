#!/usr/bin/env python3
"""tests/qemu/corrupt_elf.py MODE IN OUT - write a deliberately damaged copy of an ELF (host test fixture; never part of the kernel or the package).
Each mode exercises a different refusal in the kernel's ELF validator: magic, truncated, kernelva (a segment placed in the kernel's address range),
wx (a writable AND executable segment), entry (entry point outside every segment)."""
import struct, sys

def main():
    mode, src, dst = sys.argv[1:4]
    b = bytearray(open(src, "rb").read())
    phoff = struct.unpack_from("<Q", b, 32)[0]
    if mode == "magic": b[0] ^= 0xFF
    elif mode == "truncated": b = b[:100]
    elif mode == "kernelva":
        va = struct.unpack_from("<Q", b, phoff + 16)[0]
        struct.pack_into("<Q", b, phoff + 16, va | (0xFFFF << 48))      # low 12 bits kept, so alignment still matches: only the range check can refuse it
    elif mode == "wx": struct.pack_into("<I", b, phoff + 4, 7)           # PF_R|PF_W|PF_X
    elif mode == "entry": struct.pack_into("<Q", b, 24, 0x1000)
    else: sys.exit("corrupt_elf: unknown mode " + mode)
    open(dst, "wb").write(bytes(b))

if __name__ == "__main__":
    main()
