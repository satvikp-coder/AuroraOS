#!/usr/bin/env python3
"""tests/kernel-host/mutate_aura_cmd.py - mutation check for kernel/ui/aura_cmd.c: every mutant injects one representative defect and the host suite
(aura_cmd_test.c) must FAIL for it. Run from the repository root. Not part of CI (one compile per mutant); results are recorded in the PR."""
import os, subprocess, sys, tempfile, shutil

SRC = "kernel/ui/aura_cmd.c"
TEST = "tests/kernel-host/aura_cmd_test.c"
# Equivalent mutant (documented, expected to survive): every kind of intent checks its EXACT risk (0, 1 or 3), so the generic "risk > 3" guard in
# the validator is defence in depth that no input can reach.
EQUIVALENT = ("the validator accepts risk 4",)
BS = chr(92)
MUTANTS = [
    ("injection word list loses 'ignore'", '"ignore", "disregard",', '"disregard",'),
    ("injection scan ignores paths", "if (w[j] == '/' ||", "if (0 ||"),
    ("injection scan ignores raw addresses", "if (w[0] == '0' && w[1] == 'x') bad = 1;", ""),
    ("'previous instructions' phrase not detected", 'eq(w, "previous") && (eq(tk.t[i + 1], "instructions")', 'eq(w, "previous") && (eq(tk.t[i + 1], "xinstructions")'),
    ("destructive scan is skipped", "    if (has_any(&tk, lo, hi, DESTRUCTIVE)) return fin(o, AK_DESTRUCTIVE", "    if (0) return fin(o, AK_DESTRUCTIVE"),
    ("a deletion is reported with risk S2", 'AR_NEEDS_BROKER, 3, 0, AA_NONE, 0, "Not done', 'AR_NEEDS_BROKER, 2, 0, AA_NONE, 0, "Not done'),
    ("conjunction 'and' is not recognised", 'static const char *const CONJ[] = { "and",', 'static const char *const CONJ[] = { "xand",'),
    ("conjunction 'then' is not recognised", '"then", "also"', '"xthen", "also"'),
    ("a theme change is directly actionable", "fin(o, AK_SET_SETTING, AS_NEEDS_APPROVAL, AR_NONE, 1, ASET_THEME", "fin(o, AK_SET_SETTING, AS_ACTIONABLE, AR_NONE, 0, ASET_THEME"),
    ("a text size change is directly actionable", "fin(o, AK_SET_SETTING, AS_NEEDS_APPROVAL, AR_NONE, 1, ASET_TEXT_SCALE, AA_ABS, v, b);", "fin(o, AK_SET_SETTING, AS_ACTIONABLE, AR_NONE, 0, ASET_TEXT_SCALE, AA_ABS, v, b);"),
    ("theme values swapped (dark = 1)", 'v = 2; else if (eq(tk->t[p], "light")) v = 1;', 'v = 1; else if (eq(tk->t[p], "light")) v = 2;'),
    ("text size lower bound 74", "v < 75 || v > 200", "v < 74 || v > 200"),
    ("text size upper bound 201", "v < 75 || v > 200", "v < 75 || v > 201"),
    ("an over-long number is not rejected", "if (i >= 3) return -1;", ""),
    ("Pages ignores the platform capability", "if (sc == ASC_PAGES && !caps->pages)", "if (0)"),
    ("upper case is not folded", "if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');", ""),
    ("control characters are accepted", "(unsigned char)text[i] < 0x20 ||", ""),
    ("non-ASCII bytes are accepted", "|| (unsigned char)text[i] > 0x7e) return AR_BAD_ENCODING;", ") return AR_BAD_ENCODING;"),
    ("input length limit is 65", "if (len > AURA_CMD_MAX) return AR_TOO_LONG;", "if (len > AURA_CMD_MAX + 1) return AR_TOO_LONG;"),
    ("input length limit is 63", "if (len > AURA_CMD_MAX) return AR_TOO_LONG;", "if (len >= AURA_CMD_MAX) return AR_TOO_LONG;"),
    ("a word of 17 characters is accepted", "if (cur >= TOKLEN) return AR_TOO_LONG;", ""),
    ("leftover words are ignored (partial execution)", "    if (p != hi) return 0;                                  /* leftover words: not an exact command */", ""),
    ("a prefix of a target is not ambiguous", "if (is_prefix_of_known(tk->t[p]) && p + 1 == hi)", "if (0)"),
    ("'open' alone is not a clarification", 'fin(o, AK_CLARIFY, AS_CLARIFY, AR_MISSING_ARG, 0, 0, AA_NONE, 0, "Open what?"); return 1; }', 'return 0; }'),
    ("messaging is reported with risk S1", 'AR_NO_MESSAGING, 3, 0, AA_NONE, 0, "Unsupported: no messaging service"', 'AR_NO_MESSAGING, 1, 0, AA_NONE, 0, "Unsupported: no messaging service"'),
    ("timer requests are not recognised", 'static const char *const TIMERS[] = { "timer",', 'static const char *const TIMERS[] = { "xtimer",'),
    ("notes requests are not recognised", 'static const char *const NOTES[] = { "note", "notes",', 'static const char *const NOTES[] = { "xnote", "xnotes",'),
    ("filler 'please' is not stripped", 'static const char *const LEAD[] = { "please",', 'static const char *const LEAD[] = { "xplease",'),
    ("trailing 'now' is not stripped", 'static const char *const TRAIL[] = { "please", "now",', 'static const char *const TRAIL[] = { "please", "xnow",'),
    ("the validator accepts an actionable settings change", "if (x->status != AS_NEEDS_APPROVAL || x->reason != AR_NONE || x->risk != 1) return 0;", ""),
    ("the validator does not check previews", "static int str_eq_n(const char *a, const char *b) { int i = 0;", "static int str_eq_n(const char *a, const char *b) { return 1; int i = 0;"),
    ("the validator accepts text scale 201", "x->arg_kind != AA_ABS || x->arg < 75 || x->arg > 200", "x->arg_kind != AA_ABS || x->arg < 75 || x->arg > 201"),
    ("the validator ignores the Pages capability", "if (x->target == 4 && !caps->pages) return 0;", ""),
    ("the validator accepts risk 4", "x->risk > 3) return 0;", "x->risk > 4) return 0;"),
    ("the validator accepts an unterminated preview", "if (!terminated) return 0;", ""),
    ("the validator accepts a deletion with any status", "case AK_DESTRUCTIVE: return x->status == AS_REJECTED &&", "case AK_DESTRUCTIVE: return 1 &&"),
]

def run(src, tmp):
    c = os.path.join(tmp, "m.c"); open(c, "w").write(src)
    exe = os.path.join(tmp, "m.out")
    r = subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-I", "kernel/include", "-o", exe, TEST, c], capture_output=True, text=True)
    if r.returncode: return "COMPILE", r.stderr.splitlines()[:1]
    r = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    return ("SURVIVED" if r.returncode == 0 else "caught"), (r.stdout.count("FAIL") + r.stderr.count("runtime error"))

def main():
    orig = open(SRC).read()
    tmp = tempfile.mkdtemp()
    bad = 0
    try:
        if run(orig, tmp)[0] != "SURVIVED":
            print("the unmutated source does not pass its own suite"); return 2
        for name, old, new in MUTANTS:
            if old not in orig:
                print("NOT APPLIED: %s" % name); bad += 1; continue
            res, info = run(orig.replace(old, new, 1), tmp)
            print("%-8s %s (%s)" % (res, name, info))
            if res != "caught" and name not in EQUIVALENT: bad += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("mutants: %d, unexpected survivors or errors: %d" % (len(MUTANTS), bad))
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
