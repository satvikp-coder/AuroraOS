#!/usr/bin/env python3
"""tests/qemu/aura_s0_check.py - S0 Aura navigation through the Action Broker on the REAL shell. EL0 clients (tests/programs/uiclient.c modes 6-8) ask the
broker with SYS_UI op 3; the kernel policy answers from the kernel-attested identity and only the shell executes. Evidence: the client's own "[user pN]"
lines (what the kernel answered it), the guest's "aura:" and "shell:" lines, and QMP screendump PIXELS. Scenarios (see aura-s0-tests.sh):
  allow  holds the navigate right: Open Pages and Open Settings really happen; replay, deletion, settings, timer, notes, injection, bad nonce, oversized and
         kernel-pointer requests are refused and change nothing
  deny   holds no right: every request is refused; the screen never changes
  crash  asks and faults at once: the request is dropped, never executed"""
import argparse, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

# policy answers returned to the client: (decision << 8) | reason. decision: 0 deny 1 allow_now 2 review 3 hold 4 unavailable
# reason: 0 none, 4 replay, 6 no_right, 7 not_actionable, 8 unsupported, 10 no_review, 11 no_passport
ALLOW, REPLAY, NORIGHT, NOTACT = 0x100, 4, 6, 7
UNSUP, NOREVIEW, NOPASS = (4 << 8) | 8, (4 << 8) | 10, (4 << 8) | 11

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True); ap.add_argument("--tmp", required=True)
    ap.add_argument("--scenario", required=True); ap.add_argument("--timeout", type=float, default=480)
    a = ap.parse_args()
    passed = failed = 0
    dl = time.time() + a.timeout
    tag = "[%s] " % a.scenario
    def check(name, ok):
        nonlocal passed, failed
        if ok: passed += 1; print("  PASS: " + tag + name)
        else: failed += 1; print("  FAIL: " + tag + name)
    def log():
        with open(a.serial, "rb") as f: return f.read().decode("latin-1")
    def after_ready():
        t = log(); i = t.find("shell: ready keyboards=")
        return t[i:] if i >= 0 else ""
    def wait_for(pred, secs=60):
        end = time.time() + secs
        while time.time() < end:
            if pred(): return True
            time.sleep(0.25)
        return False
    q = Qmp(a.qmp, dl)
    check("the real kernel reached the interactive shell", wait_serial(a.serial, "shell: ready keyboards=1 pointers=1", dl))
    path = os.path.join(a.tmp, a.scenario + ".ppm")
    def shot():
        q.cmd("screendump", filename=path, format="ppm"); return read_ppm(path)
    def px(img, x, y):
        w, h, d = img; o = (y * w + x) * 3; return tuple(d[o:o + 3])
    def dark(c): return c[0] + c[1] + c[2] < 480
    def bands(img, x0, x1, y0, y1):
        n = 0; inb = False
        for y in range(y0, y1):
            row = any(dark(px(img, x, y)) for x in range(x0, x1, 2))
            if row and not inb: n += 1
            inb = row
        return n
    name = {"allow": "c_nav", "deny": "c_navbad", "crash": "c_navcrash"}[a.scenario]
    check("the client was started by the kernel loader", wait_for(lambda: ("shell: client %s pid=" % name) in log(), 120))
    m = re.search(r"shell: client %s pid=(\d+)" % name, log()); pid = int(m.group(1)) if m else 0
    if a.scenario != "deny": check("the kernel (not the client) granted the navigate right", ("shell: navigate right granted to %s" % name) in log())
    else: check("this client was NOT granted any right", "navigate right granted" not in log())
    def user_lines(): return [l for l in after_ready().split(chr(10)) if l.startswith("[user p%d] " % pid)]
    def ans(what):
        for l in user_lines():
            mm = re.match(r"\[user p%d\] %s=(-?\d+)" % (pid, re.escape(what)), l)
            if mm: return int(mm.group(1))
        return None
    def screens(): return re.findall(r"shell: screen=(\w+)", after_ready())

    if a.scenario in ("allow", "deny"):
        check("the client's whole battery ran (final request answered)", wait_for(lambda: ans("aura open settings") is not None, 180))
        ok = a.scenario == "allow"
        check("open pages: %s" % ("allowed (0x100)" if ok else "denied for lack of right (6)"), ans("aura open pages") == (ALLOW if ok else NORIGHT))
        check("the same nonce replayed is refused (4) whoever asks", ans("aura replay of nonce 1") == REPLAY)
        check("delete everything: unavailable (no Passport), never allowed", ans("aura delete everything") == NOPASS)
        check("make text bigger: denied (6): this client holds no settings right, so the request does not even reach the review question", ans("aura make text bigger") == NORIGHT)
        check("start a timer: unavailable (no timer service)", ans("aura start a timer") == UNSUP)
        check("open notes: unavailable (no Notes app)", ans("aura open notes") == UNSUP)
        check("prompt injection: refused as not actionable", ans("aura injection") == NOTACT)
        check("nonce 0 is never valid (4)", ans("aura nonce 0") == REPLAY)
        check("an oversized command is refused by the kernel (-3, E2BIG) before the broker sees it", ans("aura oversized") == -3)
        check("a kernel address as the command buffer is refused (-2, EFAULT)", ans("aura kernel pointer") == -2)
        check("open settings: %s" % ("allowed" if ok else "denied (6)"), ans("aura open settings") == (ALLOW if ok else NORIGHT))
        time.sleep(2)
        S = screens()
        if ok:
            check("the guest EXECUTED Open Pages (shell state verified): the shell screen log shows Pages", "Pages" in S and wait_for(lambda: "[Done: Open Pages] screen=Pages" in log(), 30))
            check("... and later Open Settings", wait_for(lambda: "[Done: Open Settings] screen=Settings" in log(), 30) and S.index("Pages") < len(S) and "Settings" in screens())
            check("exactly two requests were executed, both successfully", log().count("aura: executed pid=%d " % pid) == 2 and log().count("state=1 [Done: ") == 2)
            check("the shell visited only Pages and Settings (no other screen was ever opened by the refused requests)", screens() == ["Pages", "Settings"])
            img = shot()
            check("SCREEN: the Settings screen is really drawn (many text rows)", bands(img, 56, 960, 100, 640) >= 8)
        else:
            check("no request was executed and the shell never left Home", "aura: executed" not in log() and not screens())
            check("SCREEN: still the Home screen (the Current Space card is drawn)", bands(shot(), 64, 480, 100, 180) >= 2)
        check("the guest logged the broker decisions with the kernel-attested pid", ("aura: request pid=%d decision=%s" % (pid, "allow_now" if ok else "deny")) in log())
        check("text size and appearance were never touched by any request", "shell: setting row=" not in log())
    else:
        check("the request was ALLOWED by the policy and queued (client got 0x100)", wait_for(lambda: ans("aura crash ask open settings") == ALLOW, 120))
        check("the client then really faulted (the kernel contained it)", wait_for(lambda: "FAULT at EL0" in after_ready(), 60))
        check("the broker dropped the queued request: the requester was gone (state=2, Denied)", wait_for(lambda: "aura: executed pid=%d state=2 [Denied. Nothing changed.]" % pid in log(), 30))
        check("the shell did NOT navigate: still on Home, no 'Done' line", not screens() and "[Done: " not in log())
        check("SCREEN: still Home", bands(shot(), 64, 480, 100, 180) >= 2)
        b = shot(); time.sleep(0.3)
        check("the shell stays alive after the crash", "KERNEL PANIC" not in log())
    check("no panic or kernel bug", all(s not in log() for s in ("KERNEL PANIC", "KERNEL BUG", "SELFTEST FAILED")))
    print("aura_s0_check %s: %d passed, %d failed" % (a.scenario, passed, failed))
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
