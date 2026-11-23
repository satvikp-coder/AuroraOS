#!/usr/bin/env python3
"""tests/kernel-host/mutate_matvec.py - mutation check for tests/programs/matvec_core.h: each mutant injects one representative defect (a bound off
by one, a missing check, wrong rounding, lost sign); the host suite (matvec_test.c) must FAIL for every one. A mutant marked compile=True must be
refused by the compiler (the static assert that keeps the int32 accumulator from overflowing). Run from the repository root:
    python3 tests/kernel-host/mutate_matvec.py
Not part of CI (it compiles the suite once per mutant); its result is recorded in the PR / commit."""
import os, shutil, subprocess, sys, tempfile

SRC = "tests/programs/matvec_core.h"
TEST = "tests/kernel-host/matvec_test.c"
REF = "tests/kernel-host/matvec_ref.h"
# (name, [(old, new), ...], compile_error_expected)
MUTANTS = [
    ("column limit raised to 262144 (the accumulator could overflow)", [("#define MV_MAX_COLS   4096u", "#define MV_MAX_COLS   262144u")], True),
    ("column limit off by one in the header check", [("cols > MV_MAX_COLS) return MV_EDIMS;", "cols > MV_MAX_COLS + 1) return MV_EDIMS;")], False),
    ("row limit off by one in the header check", [("rows > MV_MAX_ROWS || cols > MV_MAX_COLS) return MV_EDIMS;", "rows > MV_MAX_ROWS + 1 || cols > MV_MAX_COLS) return MV_EDIMS;")], False),
    ("zero rows accepted by the header check", [("if (rows == 0 || cols == 0 || rows > MV_MAX_ROWS || cols > MV_MAX_COLS) return MV_EDIMS;", "if (cols == 0 || rows > MV_MAX_ROWS || cols > MV_MAX_COLS) return MV_EDIMS;")], False),
    ("size check accepts an object larger than the header says", [("if (mv_object_size(rows, cols) != object_size) return MV_ESIZE;", "if (mv_object_size(rows, cols) > object_size) return MV_ESIZE;")], False),
    ("size check removed", [("if (mv_object_size(rows, cols) != object_size) return MV_ESIZE;", "")], False),
    ("a short payload read is accepted", [("if (n != (int64_t)want) return MV_ETRUNC;", "if (n < 0) return MV_ETRUNC;")], False),
    ("a short header read is accepted", [("if (n != (int64_t)MV_HDR_SIZE) return MV_ETRUNC;", "")], False),
    ("reads of 8192 bytes (above SYS_MODEL's limit)", [("#define MV_CHUNK      4096u", "#define MV_CHUNK      8192u")], False),
    ("high clamp off by one", [("if (p > 127) p = 127;", "if (p > 128) p = 128;")], False),
    ("low clamp removed", [("if (p < -128) p = -128;", "")], False),
    ("negative values round toward zero", [("return -(int64_t)((m + (((uint64_t)1 << s) - 1)) >> s);", "return -(int64_t)(m >> s);")], False),
    ("no rounding offset (floor instead of round half up)", [("p = mv_floor_shift(p + ((int64_t)1 << (shift - 1)), shift);", "p = mv_floor_shift(p, shift);")], False),
    ("a zero multiplier is accepted", [("if (r->mraw == 0 || r->mraw > 0x7FFFFFFFu)", "if (r->mraw > 0x7FFFFFFFu)")], False),
    ("bytes after the last row are ignored", [("            r->err = MV_EOVER;", "            ;")], False),
    ("finish accepts an incomplete stream", [("return r->phase == 3 ? MV_OK : MV_ETRUNC;", "return MV_OK;")], False),
    ("the magic's NUL is not checked", [("for (int i = 0; i < 8; i++) if (b[i] != magic[i])", "for (int i = 0; i < 7; i++) if (b[i] != magic[i])")], False),
    ("reserved header words ignored", [("if (mv_le32(b + 24) != 0 || mv_le32(b + 28) != 0) return MV_EFORMAT;", "")], False),
    ("shift limit off by one", [("if (shift > MV_MAX_SHIFT) return MV_ESHIFT;", "if (shift > MV_MAX_SHIFT + 1) return MV_ESHIFT;")], False),
    ("table kind not checked", [("if (kind != MV_KIND) return MV_EKIND;", "")], False),
    ("table size not checked", [("if (size < MV_HDR_SIZE) return MV_ESHORT;\n    return MV_OK;", "return MV_OK;")], False),
    ("run reads a header from an object shorter than one", [("if (size < MV_HDR_SIZE) return MV_ESHORT;\n    int64_t n", "int64_t n")], False),
    ("int8 sign lost", [("return (int32_t)b - (b >= 128u ? 256 : 0);", "return (int32_t)b;")], False),
    ("accumulator not reset between rows", [("r->mult = r->mraw; r->acc = 0; r->phase = 2;", "r->mult = r->mraw; r->phase = 2;")], False),
]

def run(core_text, tmp):
    os.makedirs(os.path.join(tmp, "programs"), exist_ok=True); os.makedirs(os.path.join(tmp, "kernel-host"), exist_ok=True)
    open(os.path.join(tmp, "programs", "matvec_core.h"), "w").write(core_text)
    shutil.copy(TEST, os.path.join(tmp, "kernel-host", "matvec_test.c")); shutil.copy(REF, os.path.join(tmp, "kernel-host", "matvec_ref.h"))
    exe = os.path.join(tmp, "m.out")
    r = subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-o", exe,
                        os.path.join(tmp, "kernel-host", "matvec_test.c")], capture_output=True, text=True)
    if r.returncode: return "COMPILE", ([l for l in r.stderr.splitlines() if "error" in l] or [""])[0][-110:]
    r = subprocess.run([exe], capture_output=True, text=True, timeout=600)
    return ("SURVIVED" if r.returncode == 0 else "caught"), "%d FAIL lines, %d runtime errors" % (r.stdout.count("FAIL"), r.stderr.count("runtime error"))

def main():
    orig = open(SRC).read()
    tmp = tempfile.mkdtemp()
    bad = 0
    try:
        base, info = run(orig, tmp)
        if base != "SURVIVED":
            print("the unmutated source does not pass its own suite:", base, info); return 2
        for name, edits, want_compile in MUTANTS:
            text = orig; applied = True
            for old, new in edits:
                if text.count(old) != 1: applied = False; break
                text = text.replace(old, new, 1)
            if not applied:
                print("NOT APPLIED %s" % name); bad += 1; continue
            res, info = run(text, tmp)
            if want_compile and res == "COMPILE": res = "caught"; info = "refused at compile time: " + info
            elif want_compile: res = "SURVIVED"
            print("%-8s %s (%s)" % (res, name, info))
            if res != "caught": bad += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("mutants: %d, survivors or errors: %d" % (len(MUTANTS), bad))
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
