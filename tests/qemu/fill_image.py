#!/usr/bin/env python3
"""tests/qemu/fill_image.py MKAFS OUT SECTORS SPARE_SLOTS FILE_ARGS... - build an AuroraFS image (fixture tool, host only) and then FILL ALL ITS FREE DATA BLOCKS with filler files
owned by another principal, until no further sector fits. FILE_ARGS are mkafs's 'OWNER PERM NAME HOSTFILE' groups and are installed first. At least SPARE_SLOTS file slots are left
free, so the image is full of DATA (zero free blocks) but not full of FILE SLOTS: the failure under test is no space, not no inode. Exits non-zero if the image cannot be built."""
import os, subprocess, sys, tempfile

mkafs, out, sectors, spare = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
fixed = sys.argv[5:]
SLOTS = 32
used = len(fixed) // 4
tmp = tempfile.mkdtemp()
def build(extra):
    r = subprocess.run([mkafs, out, sectors] + fixed + extra, capture_output=True)
    return r.returncode == 0
if not build([]): sys.exit("fill_image: the fixed files do not fit")
extra, n = [], 0
for size_sectors in range(48, 0, -1):
    while used + n + 1 <= SLOTS - spare:
        p = os.path.join(tmp, "f%d" % n)
        with open(p, "wb") as f: f.write(b"\xAA" * (size_sectors * 512))
        owner = str(701 + n % 6)                                    # several owners: one principal's quota must not be what stops the fill
        if build(extra + [owner, "7", "fill%d" % n, p]):
            extra += [owner, "7", "fill%d" % n, p]; n += 1
        else:
            break
if not build(extra): sys.exit("fill_image: final build failed")
# prove it is full: one more 1-sector file must not fit
p = os.path.join(tmp, "probe"); open(p, "wb").write(b"\xAA" * 512)
probe = os.path.join(tmp, "probe.img")
for owner in range(701, 707):                                       # no owner may be able to add even one more sector: the limit really is free space
    r = subprocess.run([mkafs, probe, sectors] + fixed + extra + [str(owner), "7", "probe", p], capture_output=True)
    if r.returncode == 0: sys.exit("fill_image: the image is not full (one more sector still fits for owner %d)" % owner)
print("fill_image: %d filler files, %d fixed, image full" % (n, used))
