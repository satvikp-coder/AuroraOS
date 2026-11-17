#!/usr/bin/env python3
"""tests/qemu/clock_check.py - the native Clock screen and the completion toast on the REAL shell. Evidence: real injected key and pointer events, QMP
screendump PIXELS (the countdown digits really change, the toast is really drawn in the header, then really disappears) and the guest's own lines
("shell: screen=", "shell: focus=", "shell: say ...", "shell: toast ..."). The timer is the real SYS_TIMER service on the real monotonic counter: a
1 minute timer takes a real minute. Scenario "run" (see clock-tests.sh)."""
import argparse, math, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

AXMAX = 32767
def raw_for(p, extent): return math.ceil(p * AXMAX / (extent - 1))

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
    def says(): return [l.rstrip(chr(13))[len("shell: say "):] for l in after_ready().split(chr(10)) if l.startswith("shell: say ")]
    def toasts(): return [l.rstrip(chr(13)) for l in after_ready().split(chr(10)) if l.startswith("shell: toast ")]
    def wait_for(pred, secs=40):
        end = time.time() + secs
        while time.time() < end:
            if pred(): return True
            time.sleep(0.25)
        return False
    q = Qmp(a.qmp, dl)
    check("the real kernel reached the interactive shell", wait_serial(a.serial, "shell: ready keyboards=1 pointers=1", dl))
    check("no panic or failed self-test", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())
    path = os.path.join(a.tmp, a.scenario + ".ppm")
    def shot():
        q.cmd("screendump", filename=path, format="ppm"); return read_ppm(path)
    def px(img, x, y):
        w, h, d = img; o = (y * w + x) * 3; return tuple(d[o:o + 3])
    def region(img, x0, y0, x1, y1):
        w, h, d = img
        return b"".join(bytes(d[(y * w + x0) * 3:(y * w + x1) * 3]) for y in range(y0, y1))
    def is_ink(c): return abs(c[0] - 0x14) < 12 and abs(c[1] - 0x21) < 12 and abs(c[2] - 0x36) < 12     # the toast fill (ink 142136)
    def ink_px(img, x0, y0, x1, y1): return sum(1 for y in range(y0, y1) for x in range(x0, x1) if is_ink(px(img, x, y)))
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k): q.cmd("input-send-event", events=[key(k, True), key(k, False)]); time.sleep(0.15)
    def typ(t):
        for ch in t: tap(ch)
        time.sleep(0.5)
    def move(x, y): q.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": raw_for(x, 1024)}}, {"type": "abs", "data": {"axis": "y", "value": raw_for(y, 768)}}])
    def btn(d): q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": "left"}}])
    def click(x, y): move(x, y); time.sleep(0.5); btn(True); btn(False); time.sleep(0.7)
    def screen_now(): return (re.findall(r"shell: screen=(\w+)", log()) or [None])[-1]
    def shell_focus(): return (re.findall(r"shell: focus=(\w+)", log()) or [None])[-1]
    def tab_to(fid, tries=20):
        for _ in range(tries):
            if shell_focus() == str(fid): return True
            tap("tab"); time.sleep(0.3)
        return shell_focus() == str(fid)
    def wait_screen(name, secs=15): return wait_for(lambda: screen_now() == name, secs)
    def stable():
        a1 = shot(); time.sleep(0.4); a2 = shot()
        return a2 if region(a1, 0, 70, 1024, 660) == region(a2, 0, 70, 1024, 660) else shot()
    VAL = (56, 223, 700, 260)       # the m:ss value, 100 percent layout
    HEAD = (170, 4, 400, 62)        # the toast slot in the header

    # --- open Clock through Index (dock Index is id 4), typing its name ---
    check("Tab reaches the dock Index button", tab_to(4))
    tap("ret"); check("Enter opens Index", wait_screen("Index"))
    typ("clock"); tap("ret")
    check("typing 'clock' + Enter opens the native Clock screen", wait_screen("Clock"))
    ready = stable()
    check("the Clock card is drawn (dark text in the value row)", sum(1 for y in range(VAL[1], VAL[3]) for x in range(VAL[0], VAL[2]) if sum(px(ready, x, y)) < 300) > 80)
    check("no toast before any timer finishes", ink_px(ready, *HEAD) < 50)
    # --- keyboard: choose 5 min, start, running, cancel ---
    check("Tab reaches the 5 min preset (id 251)", tab_to(251))
    tap("ret")
    check("choosing 5 min is spoken", wait_for(lambda: any("5 min selected" in s for s in says()), 10))
    five = stable()
    check("the value row changed to 5:00 on screen", region(five, *VAL) != region(ready, *VAL))
    check("Tab reaches Start (id 253)", tab_to(253))
    tap("ret")
    check("Start is spoken as running", wait_for(lambda: any(s.startswith("Running") for s in says()), 10))
    check("Start moved focus to Cancel (id 254)", wait_for(lambda: shell_focus() == "254", 10))
    r1 = stable(); time.sleep(2.3); r2 = stable()
    check("the countdown really changes on screen (two screenshots 2.3 s apart differ in the value row)", region(r1, *VAL) != region(r2, *VAL))
    tap("ret")
    check("Enter on the focused Cancel cancels: spoken", wait_for(lambda: any(s.startswith("Cancelled") for s in says()), 10))
    c1 = stable(); time.sleep(2.3); c2 = stable()
    check("after Cancel the value row stops changing", region(c1, *VAL) == region(c2, *VAL))
    check("a cancelled timer never produced a toast", not toasts())
    # --- pointer: pick 1 min, Start, leave for Home, and let it finish off-screen ---
    click(200, 320)       # 1 min preset
    check("pointer: choosing 1 min is spoken", wait_for(lambda: any("1 min selected" in s for s in says()), 10))
    t_start = time.time()
    click(280, 376)       # Start
    check("pointer: Start runs the timer", wait_for(lambda: sum(1 for s in says() if s.startswith("Running")) >= 2, 10))
    click(150, 459)       # Home button of the Clock card
    check("pointer: the Home button leaves the Clock", wait_screen("Home"))
    check("no toast while it is still running", ink_px(stable(), *HEAD) < 50)
    ok = wait_for(lambda: any("on" in t for t in toasts()), 100)
    waited = time.time() - t_start
    check("the timer finished while the Clock was NOT on screen: a toast was logged (%.0f s after Start)" % waited, ok)
    check("it was never early: at least 58 s after Start (the 1 minute timer)", waited >= 58)
    check("it finished promptly (under 80 s)", waited < 80)
    t1 = stable()
    check("the toast is really drawn in the header over the Horizon slot (ink-coloured pixels)", ink_px(t1, *HEAD) > 800)
    check("the toast did not take focus or navigate: still on Home", screen_now() == "Home")
    check("the toast is gone about 4 s later (pixels) and its removal is logged", wait_for(lambda: ink_px(shot(), *HEAD) < 50, 12) and wait_for(lambda: any("off" in t for t in toasts()), 5))
    click(920, 150) if False else None
    check("finished state was spoken", any(s.startswith("Finished") for s in says()))
    check("no panic at the end", "KERNEL PANIC" not in log())
    print("clock_check %s: %d passed, %d failed" % (a.scenario, passed, failed))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
