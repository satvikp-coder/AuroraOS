#!/usr/bin/env python3
"""tests/qemu/app_check.py - the native launcher and Calculator (see app-tests.sh). Real QMP key/pointer events, screendump pixels and the guest's own lines."""
import argparse, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

SID_DOCK_INDEX = 4
SID_DOCK_HOME = 2
SID_APP_AURA = 14

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True); ap.add_argument("--tmp", required=True)
    ap.add_argument("--scenario", required=True); ap.add_argument("--expect", default=""); ap.add_argument("--timeout", type=float, default=400)
    ap.add_argument("--check-aura-notes", action="store_true")
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
    def lines(prefix): return [l.rstrip(chr(13)) for l in after_ready().split(chr(10)) if l.startswith(prefix)]
    def wait_for(pred, secs=40):
        end = time.time() + secs
        while time.time() < end:
            if pred(): return True
            time.sleep(0.2)
        return False
    q = Qmp(a.qmp, dl)
    check("the real kernel reached the interactive shell", wait_serial(a.serial, "shell: ready keyboards=1 pointers=1", dl))
    check("no panic or failed self-test", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())
    path = os.path.join(a.tmp, a.scenario + ".ppm")
    def shot():
        q.cmd("screendump", filename=path, format="ppm"); return read_ppm(path)
    def region(img, x0, y0, x1, y1):
        w, h, d = img
        return b"".join(bytes(d[(y * w + x0) * 3:(y * w + x1) * 3]) for y in range(y0, y1))
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k): q.cmd("input-send-event", events=[key(k, True), key(k, False)]); time.sleep(0.25)
    def typ(t):
        for ch in t: tap("spc" if ch == " " else ch)
        time.sleep(0.4)
    def move(x, y): q.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 1023)}}, {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 767)}}]); time.sleep(0.2)
    def btn(d): q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": "left"}}])
    def click(x, y): move(x, y); time.sleep(0.5); btn(True); btn(False); time.sleep(0.7)
    def focus_now(): return (re.findall(r"shell: focus=(\w+)", log()) or [None])[-1]
    def screen_now(): return (re.findall(r"shell: screen=(\w+)", log()) or [None])[-1]
    def tab_to(fid, tries=24):
        for _ in range(tries):
            if focus_now() == str(fid): return True
            tap("tab"); time.sleep(0.3)
        return focus_now() == str(fid)
    def open_index():
        ok = tab_to(SID_DOCK_INDEX); tap("ret"); return ok and wait_for(lambda: screen_now() == "Index", 15)
    def launched(): return re.findall(r"apps: launched calculator pid=(\d+) principal=(\d+)", log())
    def surfaces(): return [(int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6))) for m in re.finditer(r"ui: surface slot=(\d+) pid=(\d+) at (\d+),(\d+) size (\d+)x(\d+)", log())]
    def user(pid): return [l for l in after_ready().split(chr(10)) if l.startswith("[user p%d] " % pid)]
    sc = a.scenario

    if sc == "launch":
        check("boot registered the Calculator in the kernel registry and granted its UI capability", "apps: calculator registered ui=granted" in log())
        check("Index opens", open_index())
        typ("calc")
        tap("ret")
        check("Enter makes the KERNEL launch it: pid assigned, kernel-chosen identity 601 (the person typed no identity)", wait_for(lambda: len(launched()) == 1, 30) and launched()[0][1] == "601")
        pid = int(launched()[0][0]) if launched() else 0
        check("the shell shows Pages after a launch", wait_for(lambda: screen_now() == "Pages", 15))
        check("Pages lists the running calculator process", wait_for(lambda: any(("%d/calculator/" % pid) in l for l in lines("shell: pages n=")), 30))
        check("the EL0 process started and drew its first state (display 0)", wait_for(lambda: any("calc display=0" in l for l in user(pid)), 30))
        check("the compositor accepted and PLACED its surface itself", wait_for(lambda: any(s[1] == pid for s in surfaces()), 30))
        s = [s for s in surfaces() if s[1] == pid]
        sx, sy, sw, sh_ = (s[0][2], s[0][3], s[0][4], s[0][5]) if s else (0, 0, 0, 0)
        click(sx + sw // 2, sy + sh_ // 2)                                            # a press on the surface gives it keyboard focus
        before = shot()
        for k in ("1", "2"): tap(k)
        tap("kp_add"); tap("8"); tap("kp_enter")
        check("12 + 8 = 20: the process saw real keys and computed it", wait_for(lambda: any("calc display=20" in l for l in user(pid)), 30))
        check("SCREEN: its surface changed on the real display", region(shot(), sx, sy, sx + sw, sy + sh_) != region(before, sx, sy, sx + sw, sy + sh_))
        tap("c")
        check("C clears it (display 0 again)", wait_for(lambda: [l for l in user(pid) if "calc display=" in l][-1:] and "calc display=0" in [l for l in user(pid) if "calc display=" in l][-1], 20))
        tap("1"); tap("slash"); tap("0"); tap("kp_enter")
        check("1 / 0 shows the error, never a crash", wait_for(lambda: any("calc display=Divide by zero" in l for l in user(pid)), 20))
        check("an unrelated process (the shell) did not launch a second copy", len(launched()) == 1)
        check("open Index again and ask for the Calculator while it runs", tab_to(SID_DOCK_INDEX) and (tap("ret") or True) and wait_for(lambda: screen_now() == "Index", 15))
        typ("calc"); tap("ret"); time.sleep(1.0)
        check("a second launch while it is open is refused: still one instance, still Index", len(launched()) == 1 and screen_now() == "Index")
        tap("esc"); time.sleep(0.5); tap("esc")                                       # Esc clears the search, then goes back to Pages (where the surface is reachable)
        check("Esc goes back to Pages", wait_for(lambda: screen_now() == "Pages", 15))
        click(sx + sw // 2, sy + sh_ // 2); tap("q")
        check("Q exits the Calculator (exit code 0)", wait_for(lambda: any(("shell: reaped pid=%d exited code=0" % pid) in l for l in lines("shell: reaped")), 30))
        check("cleanup: the surface and the launcher's record are gone", wait_for(lambda: any(("ui: client gone pid=%d" % pid) in l for l in lines("ui: client gone")), 20) and "apps: instance gone calculator" in log())
        check("Index opens again", open_index())
        typ("calc"); tap("ret")
        check("relaunch starts a FRESH instance (new launch, same identity), not a resurrection", wait_for(lambda: len(launched()) == 2 and launched()[1][1] == "601", 30))
        pid2 = int(launched()[1][0]) if len(launched()) > 1 else 0
        def since_relaunch():                                                           # the pid slot may be reused, so only lines AFTER the second launch belong to the new instance
            t2 = after_ready(); marks = [m.start() for m in re.finditer(r"apps: launched calculator", t2)]
            return [l for l in t2[marks[1]:].split(chr(10)) if l.startswith("[user p%d] " % pid2)] if len(marks) > 1 else []
        check("the new instance starts at 0: no stale display or state from the old one", wait_for(lambda: any("calc display=0" in l for l in since_relaunch()), 30) and not any("calc display=20" in l or "Divide" in l for l in since_relaunch()))
        check("exactly one new surface was created for it (the old one is gone)", len([s for s in surfaces() if s[1] == pid2]) == 2 and "ui: client gone pid=%d surfaces dropped=1" % pid in log())
        if a.check_aura_notes:
            check("the packaged shell can return Home and open Aura", tab_to(SID_DOCK_HOME) and (tap("ret") or True) and
                  wait_for(lambda: screen_now() == "Home", 15) and tab_to(SID_APP_AURA) and (tap("ret") or True) and
                  wait_for(lambda: screen_now() == "Aura", 15))
            typ("open notes"); tap("ret")
            check("Aura executes the typed Notes launch and opens Pages", wait_for(lambda:
                  "kind=open_notes decision=allow_now reason=none state=1 [Done: Open Notes] screen=Pages" in log(), 30))
            notes = re.search(r"apps: launched notes pid=(\d+) principal=602", log())
            check("the packaged Notes process uses the registry identity and draws a client surface", notes is not None and
                  wait_for(lambda: any(s[1] == int(notes.group(1)) for s in surfaces()), 30))
    elif sc == "refused":
        # An installed Calculator that is damaged: the kernel must refuse it with a specific reason, create no process, and leave the other applications usable.
        check("boot registered the Calculator", "apps: calculator registered" in log())
        check("Index opens", open_index())
        typ("calc")
        tap("ret")
        check("the launcher refused with the expected reason (%s)" % a.expect, wait_for(lambda: re.search(r"apps: launch failed: (%s)" % a.expect, log()) is not None, 30))
        check("no Calculator process was created and nothing was launched", not launched() and "apps: launched" not in log())
        check("no surface was created by the refused launch", not surfaces())
        check("the shell stayed on Index and is still interactive", screen_now() == "Index")
        for _ in range(8): tap("backspace")
        typ("notes")
        tap("ret")
        check("another installed application (Notes) still launches", wait_for(lambda: "apps: launched notes" in log(), 30))
    else:
        check("boot registered the Calculator", "apps: calculator registered" in log())
        check("Index opens", open_index())
        typ("calc")
        tap("ret"); time.sleep(1.0)
        check("Enter launches nothing: no launch line, still on Index, no process created", not launched() and screen_now() == "Index" and "apps: launch failed" not in log())
    check("no panic at the end", "KERNEL PANIC" not in log())
    print("app_check %s: %d passed, %d failed" % (sc, passed, failed))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
