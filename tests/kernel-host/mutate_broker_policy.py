#!/usr/bin/env python3
"""tests/kernel-host/mutate_broker_policy.py - mutation check for kernel/ui/broker_policy.c: every mutant injects one representative defect and the
host suite (broker_policy_test.c) must FAIL for it. Run from the repository root. Not part of CI (one compile per mutant); results are recorded in the PR."""
import os, subprocess, sys, tempfile, shutil

SRC = "kernel/ui/broker_policy.c"
TEST = "tests/kernel-host/broker_policy_test.c"
AURA = "kernel/ui/aura_cmd.c"
# Documented equivalents (defence in depth the validator makes unreachable): the policy re-checks the schema version, that an actionable intent has the
# policy's own risk 0, that an approval intent has risk 1, and masks unknown right bits, but aura_validate() already guarantees the first three and each
# right is tested bit by bit, so no input can tell these guards from their absence. They are kept on purpose.
EQUIVALENT = ("the schema version is not checked here", "unknown right bits are honoured", "an actionable status is trusted without the policy's own risk", "the approval status is trusted without the policy's own risk")
MUTANTS = [
    ("the schema version is not checked here", "req->intent.version != AURA_INTENT_VERSION || !aura_validate", "!aura_validate"),
    ("the intent is not validated", "|| !aura_validate(&req->intent, env->caps)", ""),
    ("the generation is not compared", "if (gen == 0 || gen != req->gen)", "if (gen == 0)"),
    ("a stale generation is reported as a bad requester", "gen == 0 ? BPR_BAD_REQUESTER : BPR_STALE_REQUESTER", "BPR_BAD_REQUESTER"),
    ("generation 0 is accepted", "if (gen == 0 || gen != req->gen)", "if (gen != req->gen)"),
    ("a dead requester is accepted", "if (!env->lookup(req->pid, &gen, &principal, &rights)) {", "if (0) {"),
    ("unknown right bits are honoured", "if (rights & ~BP_RIGHTS_ALL) rights &= BP_RIGHTS_ALL;", ""),
    ("S0 navigation does not need the navigate right", "if (!(rights & BP_RIGHT_NAVIGATE)) {", "if (0) {"),
    ("any right opens navigation", "if (!(rights & BP_RIGHT_NAVIGATE)) {", "if (!rights) {"),
    ("S0 ignores the navigation service", "if (!env->nav_service) {", "if (0) {"),
    ("S1 does not need the settings right", "if (!(rights & BP_RIGHT_SETTINGS)) {", "if (0) {"),
    ("S1 does not need the trusted review path (an unverified approval)", "if (!env->review_path) { res(out, BP_UNAVAILABLE, BPR_NO_REVIEW, risk, principal); return; }", ""),
    ("S1 is allowed immediately", "res(out, BP_REVIEW, BPR_NONE, risk, principal); return;", "res(out, BP_ALLOW_NOW, BPR_NONE, risk, principal); return;"),
    ("S3 does not need Passport", "if (!env->passport) {", "if (0) {"),
    ("S3 does not need Trail", "if (!env->trail) {", "if (0) {"),
    ("S3 does not need the review path", "if (!env->review_path) { res(out, BP_UNAVAILABLE, BPR_NO_REVIEW, 3, principal); return; }", ""),
    ("S3 does not need hold-to-confirm", "if (!env->hold_input) {", "if (0) {"),
    ("S3 with every dependency is allowed immediately", "res(out, BP_HOLD, BPR_NONE, 3, principal); return;", "res(out, BP_ALLOW_NOW, BPR_NONE, 3, principal); return;"),
    ("unsupported requests are denied instead of reported unavailable", "res(out, BP_UNAVAILABLE, BPR_UNSUPPORTED,", "res(out, BP_DENY, BPR_UNSUPPORTED,"),
    ("an unsupported request is allowed", "res(out, BP_UNAVAILABLE, BPR_UNSUPPORTED, risk == 255 ? 0 : risk, principal); return;", "res(out, BP_ALLOW_NOW, BPR_UNSUPPORTED, risk == 255 ? 0 : risk, principal); return;"),
    ("a non-actionable intent is allowed", "default:                                                                                   /* clarify, or anything unexpected */\n        res(out, BP_DENY,", "default:\n        res(out, BP_ALLOW_NOW,"),
    ("an actionable status is trusted without the policy's own risk", "if (risk != 0) { res(out, BP_DENY, BPR_BAD_INTENT, risk, principal); return; }          /* only S0 navigation can be actionable */", ""),
    ("the approval status is trusted without the policy's own risk", "if (risk != 1) { res(out, BP_DENY, BPR_BAD_INTENT, risk, principal); return; }", ""),
    ("nonce 0 is accepted", "if (nonce == 0) return 0;", ""),
    ("an equal nonce is accepted (replay)", "if (nonce <= r->last[i]) return 0;", "if (nonce < r->last[i]) return 0;"),
    ("an older nonce is accepted", "if (nonce <= r->last[i]) return 0;", ""),
    ("nonces are not per requester", "if (r->pid[i] != pid) continue;", "if (0) continue;"),
    ("a new generation of the same pid keeps the old nonce", "r->gen[i] = gen; r->last[i] = nonce; return 1;       /* the same pid", "r->gen[i] = gen; if (nonce <= r->last[i]) return 0; r->last[i] = nonce; return 1;       /* the same pid"),
    ("the replay table is consumed before the identity is verified", "    /* 3. replay:", "    { int f0; replay_take(replay, req->pid, req->gen, req->nonce, &f0); }\n    /* 3. replay:"),
    ("a full replay table is accepted", "if (free_slot < 0) { *full = 1; return 0; }", "if (free_slot < 0) { *full = 1; return 1; }"),
    ("the verified principal is not returned", "o->decision = d; o->reason = why; o->risk = risk; o->principal = principal;", "o->decision = d; o->reason = why; o->risk = risk; o->principal = 0;"),
    ("the risk class of a setting is S0", "case AK_SET_SETTING: return 1;", "case AK_SET_SETTING: return 0;"),
    ("the risk class of a deletion is S1", "case AK_DESTRUCTIVE: return 3;", "case AK_DESTRUCTIVE: return 1;"),
    ("a deletion is treated as a plain rejection", "if (kind == AK_DESTRUCTIVE) {", "if (0) {"),
    ("missing lookup is tolerated", "if (!env || !replay || !req || !env->caps || !env->lookup) return;", "if (!env || !replay || !req || !env->caps) return;"),
]

def run(src, tmp):
    c = os.path.join(tmp, "m.c"); open(c, "w").write(src)
    exe = os.path.join(tmp, "m.out")
    r = subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-I", "kernel/include", "-o", exe, TEST, c, AURA], capture_output=True, text=True)
    if r.returncode: return "COMPILE", r.stderr.splitlines()[:1]
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return "caught", "timeout"
    return ("SURVIVED" if r.returncode == 0 else "caught"), (r.stdout.count("FAIL") + r.stderr.count("runtime error") + r.stdout.count("mismatch"))

def main():
    orig = open(SRC, newline="").read().replace("\r\n", "\n")
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
