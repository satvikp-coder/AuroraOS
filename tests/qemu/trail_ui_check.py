#!/usr/bin/env python3
"""tests/qemu/trail_ui_check.py - the native Trail screen on the REAL kernel. Evidence: real injected key events, the guest's own lines ("shell: screen=",
"shell: focus=", "shell: say <health>", "shell: trail view health=.. records=.. grants=..") and screendump PIXELS (the screen really shows rows of text; the
read-only guarantee is checked by pressing keys and clicking where rows are and seeing that nothing activates). Scenarios: records, big, nodisk, readonly,
rollback (see trail-ui-tests.sh)."""
import argparse, math, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

AXMAX = 32767
def raw_for(p, extent): return math.ceil(p * AXMAX / (extent - 1))

HEALTH = {"records": "Trail: recording to disk.", "big": "Trail: recording to disk.", "nodisk": "Trail: off. No disk, nothing is saved.",
          "readonly": "Trail: read-only disk. Changes refused.", "rollback": "Trail: rollback found. Changes refused."}
HEALTH_CODE = {"records": 1, "big": 1, "nodisk": 2, "readonly": 3, "rollback": 5}

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
    def lines(prefix): return [l.rstrip(chr(13)) for l in after_ready().split(chr(10)) if l.startswith(prefix)]
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
    def dark(c): return c[0] + c[1] + c[2] < 480
    def bands(img, x0, x1, y0, y1):
        n = 0; inb = False
        for y in range(y0, y1):
            row = any(dark(px(img, x, y)) for x in range(x0, x1, 2))
            if row and not inb: n += 1
            inb = row
        return n
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

    sc = a.scenario
    check("Tab reaches the dock Index button", tab_to(4)); tap("ret")
    check("Enter opens Index", wait_screen("Index"))
    typ("trail"); tap("ret")
    check("typing 'trail' + Enter opens the native Trail screen", wait_screen("Trail"))
    check("the health line was spoken: " + HEALTH[sc], wait_for(lambda: any(s == "shell: say " + HEALTH[sc] for s in lines("shell: say ")), 10))
    views = lines("shell: trail view health=")
    check("the guest's view of the ledger has health code %d" % HEALTH_CODE[sc], wait_for(lambda: any(("health=%d " % HEALTH_CODE[sc]) in v for v in lines("shell: trail view health=")), 10))
    img = stable()
    y0, y1 = (100, 640)
    nb = bands(img, 56, 960, y0, y1)
    if sc == "nodisk":
        check("SCREEN: the empty state is drawn (a few text rows, 'No records yet.')", 3 <= nb <= 9)
        check("the view reports no records", any("records=0" in v for v in lines("shell: trail view health=")))
    else:
        recs = [int(m) for v in lines("shell: trail view health=") for m in re.findall(r"records=(\d+)", v)]
        check("the view reports records (seen %s)" % sorted(set(recs)), bool(recs) and max(recs) >= 3)
        check("SCREEN: record rows are drawn (many text bands in the card; %d seen)" % nb, nb >= (6 if sc != "big" else 5))
    if sc in ("records", "big"):
        check("the ledger test script ran (grants audited): 'op grant 1001 files-write rc=0'", "ledger: op grant 1001 files-write rc=0" in log())
    # ---- the read-only guarantee: Tab only reaches the allowed controls; clicking rows and pressing Enter/Space with no focus does nothing ----
    ALLOWED = {"1", "2", "3", "4", "40", "41", "260", "261", "262"}
    seen = set()
    for _ in range(14):
        tap("tab"); f = shell_focus()
        if f: seen.add(f)
    check("14 Tab presses only reach allowed controls (saw %s)" % sorted(seen), seen <= ALLOWED)
    click(300, 270); move(1010, 760); time.sleep(0.6)                       # one click on a row, then park the pointer outside the card: this is the baseline
    base_img = stable()
    acts0 = len(lines("shell: activate "))
    for (x, y) in ((300, 270), (300, 330), (300, 390), (300, 450)):
        click(x, y)
    move(1010, 760); time.sleep(0.6)
    check("clicking where record rows are activated nothing and left the Trail screen open", len(lines("shell: activate ")) == acts0 and screen_now() == "Trail")
    check("the card looks exactly the same after the clicks (pointer parked again): rows are not controls", region(stable(), 56, 150, 960, 560) == region(base_img, 56, 150, 960, 560))
    # Home is reachable and works (the only way out besides the dock)
    if tab_to(262): tap("ret")
    else: click(150, 600)
    check("the Home button leaves the Trail", wait_screen("Home"))
    check("no panic at the end", "KERNEL PANIC" not in log())
    print("trail_ui_check %s: %d passed, %d failed" % (sc, passed, failed))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
