#!/usr/bin/env python3
"""tests/qemu/hold_check.py - the hold-to-confirm DEMO on the REAL shell (see hold-tests.sh). Evidence: real injected key and pointer events (a key or button is really held
down for real time), QMP screendump PIXELS (the four segments really fill) and the guest's own lines ("shell: say ...", "shell: hold confirmed total=N").
Real time is measured by this script's clock; the guest's monotonic counter decides the confirmation. The demo confirms nothing real: no action exists behind it."""
import argparse, math, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

AXMAX = 32767
def raw_for(p, extent): return math.ceil(p * AXMAX / (extent - 1))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True); ap.add_argument("--tmp", required=True)
    ap.add_argument("--timeout", type=float, default=480)
    a = ap.parse_args()
    passed = failed = 0
    dl = time.time() + a.timeout
    def check(name, ok):
        nonlocal passed, failed
        if ok: passed += 1; print("  PASS: [hold] " + name)
        else: failed += 1; print("  FAIL: [hold] " + name)
    def log():
        with open(a.serial, "rb") as f: return f.read().decode("latin-1")
    def after_ready():
        t = log(); i = t.find("shell: ready keyboards=")
        return t[i:] if i >= 0 else ""
    def says(): return [l.rstrip(chr(13))[len("shell: say "):] for l in after_ready().split(chr(10)) if l.startswith("shell: say ")]
    def confirmed(): return len(re.findall(r"shell: hold confirmed total=(\d+)", after_ready()))
    def wait_for(pred, secs=40):
        end = time.time() + secs
        while time.time() < end:
            if pred(): return True
            time.sleep(0.1)
        return False
    q = Qmp(a.qmp, dl)
    check("the real kernel reached the interactive shell", wait_serial(a.serial, "shell: ready keyboards=1 pointers=1", dl))
    path = os.path.join(a.tmp, "hold.ppm")
    def shot():
        q.cmd("screendump", filename=path, format="ppm"); return read_ppm(path)
    def px(img, x, y):
        w, h, d = img; o = (y * w + x) * 3; return tuple(d[o:o + 3])
    def is_ink(c): return abs(c[0] - 0x14) < 12 and abs(c[1] - 0x21) < 12 and abs(c[2] - 0x36) < 12     # a filled segment (ink 142136)
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k): q.cmd("input-send-event", events=[key(k, True), key(k, False)]); time.sleep(0.15)
    def typ(t):
        for ch in t: tap(ch)
        time.sleep(0.5)
    def move(x, y): q.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": raw_for(x, 1024)}}, {"type": "abs", "data": {"axis": "y", "value": raw_for(y, 768)}}]); time.sleep(0.2)
    def btn(d): q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": "left"}}])
    def screen_now(): return (re.findall(r"shell: screen=(\w+)", log()) or [None])[-1]
    def shell_focus(): return (re.findall(r"shell: focus=(\w+)", log()) or [None])[-1]
    def tab_to(fid, tries=24):
        for _ in range(tries):
            if shell_focus() == str(fid): return True
            tap("tab"); time.sleep(0.3)
        return shell_focus() == str(fid)
    def layout():
        t = log(); j = t.rfind("shell: layout end"); i = t.rfind("shell: layout begin", 0, j) if j >= 0 else -1
        c = {}
        if i < 0: return c
        for line in t[i:j].split(chr(10)):
            if line.startswith("shell: ctrl "):
                v = line.split()[2:]; c[int(v[0])] = tuple(int(x) for x in v[1:5])
        return c
    def seg_ink():
        x, y, w, h = layout()[270]
        img = shot(); return sum(1 for yy in range(y + h + 8, y + h + 20) for xx in range(x, x + w) if is_ink(px(img, xx, yy)))

    # --- open the demo through Index by typing its name ---
    check("Tab reaches the dock Index button", tab_to(4))
    tap("ret"); check("Enter opens Index", wait_for(lambda: screen_now() == "Index", 15))
    typ("hold"); tap("ret")
    check("the Hold screen opens", wait_for(lambda: screen_now() == "Hold", 15))
    check("it speaks its ready line", wait_for(lambda: "Hold for 1.5 seconds to confirm." in says(), 10))
    check("the hold button is a real control in the layout", wait_for(lambda: 270 in layout(), 10))
    check("no segment is filled at the start (pixels)", seg_ink() == 0)
    check("Tab reaches the hold button", tab_to(270))

    # --- early release cancels ---
    n0 = len(says())
    q.cmd("input-send-event", events=[key("ret", True)]); time.sleep(0.5); q.cmd("input-send-event", events=[key("ret", False)])
    check("releasing after 0.5 s cancels honestly", wait_for(lambda: "Released early. Nothing happened." in says()[n0:], 10))
    time.sleep(2.0)
    check("a cancelled hold never confirms later", confirmed() == 0)

    # --- a full keyboard hold confirms exactly once, after about 1.5 s of real time ---
    n0 = len(says()); t0 = time.time()
    q.cmd("input-send-event", events=[key("ret", True)])
    ok = wait_for(lambda: "Confirmed." in says()[n0:], 12); dt = time.time() - t0
    seg = says()[n0:]
    check("the hold confirms while the key is still held", ok)
    check("it did not confirm early: at least 1.2 s of real time passed", ok and dt >= 1.2)
    check("progress was announced on the way (Holding: 1 of 4 before Confirmed.)", "Holding: 1 of 4" in seg and (not ok or seg.index("Holding: 1 of 4") < seg.index("Confirmed.")))
    check("exactly one confirmation was logged", confirmed() == 1)
    time.sleep(0.5)
    check("all four segments are drawn filled (pixels)", seg_ink() > 4 * 60 * 6)
    time.sleep(1.5)
    check("holding longer does not confirm again", confirmed() == 1)
    q.cmd("input-send-event", events=[key("ret", False)]); time.sleep(0.8)
    check("releasing after the confirmation does not confirm again", confirmed() == 1)

    # --- a pointer hold ---
    x, y, w, h = layout()[270]; cx, cy = x + w // 2, y + h // 2
    move(cx, cy); time.sleep(0.4); n0 = len(says())
    btn(True)
    check("a pointer hold confirms", wait_for(lambda: "Confirmed." in says()[n0:] and confirmed() == 2, 12))
    btn(False); time.sleep(0.8)
    check("the pointer release does not confirm again", confirmed() == 2)

    # --- leaving the button cancels and re-entering does not resume ---
    move(cx, cy); time.sleep(0.4); n0 = len(says())
    btn(True); time.sleep(0.5); move(5, 5); time.sleep(0.3)
    check("moving the pointer out of the button cancels", wait_for(lambda: "Released early. Nothing happened." in says()[n0:], 10))
    move(cx, cy); time.sleep(2.2)
    check("moving back in does not resume it", confirmed() == 2)
    btn(False); time.sleep(0.4)

    # --- a focus change cancels ---
    tab_to(270); time.sleep(0.4); n0 = len(says())
    q.cmd("input-send-event", events=[key("ret", True)]); time.sleep(0.5)
    tap("tab")
    check("Tab during a hold cancels it", wait_for(lambda: "Released early. Nothing happened." in says()[n0:], 10))
    time.sleep(2.0)
    check("... and it never confirms afterwards", confirmed() == 2)
    q.cmd("input-send-event", events=[key("ret", False)]); time.sleep(0.5)

    # --- Escape leaves the screen ---
    tab_to(270); q.cmd("input-send-event", events=[key("ret", True)]); time.sleep(0.4)
    tap("esc")
    check("Escape during a hold leaves the demo", wait_for(lambda: screen_now() != "Hold", 10))
    q.cmd("input-send-event", events=[key("ret", False)]); time.sleep(1.0)
    check("... without confirming", confirmed() == 2)
    check("no panic, fault or failed self-test was logged", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())
    print("hold_check: %d passed, %d failed" % (passed, failed))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
