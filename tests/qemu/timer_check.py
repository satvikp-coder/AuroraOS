#!/usr/bin/env python3
"""tests/qemu/timer_check.py - SYS_TIMER evidence for tests/qemu/timer-tests.sh. The guest's EL0 clients print "[user pN] tm: PASS|FAIL <what>" for every
assertion they make about the kernel's answers; this checker (a) waits for each client to finish, (b) requires every expected PASS line and no FAIL line,
(c) proves with REAL injected key events and screendump pixels that the shell stayed interactive while timers ran and expired."""
import argparse, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

EXPECT = {   # scenario -> {client done marker: minimum PASS lines from that client}
    "basic": {"tm: done basic": 22, "tm: done nocap": 1},
    "hostile": {"tm: done hostile": 21},
    "owners": {"tm: done holder": 1, "tm: done spy": 3, "tm: done exiter": 0, "tm: faulting": 0, "tm: done late": 2},
    "starved": {"tm: done basic": 22},
}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True); ap.add_argument("--tmp", required=True)
    ap.add_argument("--scenario", required=True); ap.add_argument("--timeout", type=float, default=420)
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
    def lines(): return [x.rstrip(chr(13)) for x in after_ready().split(chr(10))]
    q = Qmp(a.qmp, dl)
    check("the real kernel reached the interactive shell", wait_serial(a.serial, "shell: ready keyboards=", dl))
    check("no panic or failed self-test", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())
    check("no capability grant failed", "timersys_grant failed" not in log())
    path = os.path.join(a.tmp, a.scenario + ".ppm")
    def shot():
        q.cmd("screendump", filename=path, format="ppm"); return read_ppm(path)
    def region(img, x0, y0, x1, y1):
        w, h, d = img
        return b"".join(bytes(d[(y * w + x0) * 3:(y * w + x1) * 3]) for y in range(y0, y1))
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k): q.cmd("input-send-event", events=[key(k, True), key(k, False)])
    def tab_changes_screen():
        before = region(shot(), 0, 64, 1024, 768); t0 = time.time(); tap("tab")
        while time.time() - t0 < 12:
            if region(shot(), 0, 64, 1024, 768) != before: return True
        return False

    def all_done(markers):
        t = "\n".join(lines())
        return all(m in t for m in markers)
    markers = list(EXPECT[a.scenario].keys())
    end = time.time() + 120
    while time.time() < end and not all_done(markers):
        if a.scenario in ("hostile",) : pass
        time.sleep(0.5)
    check("every timer client finished: " + ", ".join(markers), all_done(markers))
    tm_lines = [l for l in lines() if l.startswith("[user p") and "] tm: " in l]
    for l in tm_lines: print("    " + l)
    fails = [l for l in tm_lines if "tm: FAIL" in l]
    check("no client reported a FAIL (%d lines seen)" % len(tm_lines), not fails)
    for l in fails: print("    " + l)
    for marker, minpass in EXPECT[a.scenario].items():
        # PASS lines are printed by the process that printed the marker: attribute by pid
        pids = [l[len("[user p"):l.index("]")] for l in tm_lines if l.endswith(marker)]
        for pid in pids:
            n = len([l for l in tm_lines if l.startswith("[user p%s]" % pid) and "tm: PASS" in l])
            check("pid %s (%s) made at least %d passing assertions (saw %d)" % (pid, marker.split()[-1], minpass, n), n >= minpass)
    if a.scenario == "basic":
        check("the no-capability client was refused: it printed EPERM (-7) results", any("nocap start=-7" in l for l in tm_lines))
    if a.scenario in ("hostile", "basic", "owners", "starved"):
        check("the shell stays interactive: Tab changes the screen after the timers ran (and expired)", tab_changes_screen())
    if a.scenario == "owners":
        check("the faulting client was reaped", any("faulted" in l for l in lines() if l.startswith("shell: reaped")))
        check("the exiting client was reaped", any("exited code=0" in l for l in lines() if l.startswith("shell: reaped")))
    check("no panic at the end", "KERNEL PANIC" not in log())
    print("timer_check %s: %d passed, %d failed" % (a.scenario, passed, failed))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
