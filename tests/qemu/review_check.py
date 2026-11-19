#!/usr/bin/env python3
"""tests/qemu/review_check.py - the trusted review modal on the REAL kernel (see review-tests.sh). Four reviews are opened by the test kernel; this drives them with real
QMP key/pointer events and verifies them on screendump pixels and the guest's own lines. S1 execution is disabled: no review can change a setting."""
import argparse, math, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

AXMAX = 32767
def raw_for(p, extent): return math.ceil(p * AXMAX / (extent - 1))
SECURE = (0xFF, 0xB0, 0x20)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True); ap.add_argument("--tmp", required=True)
    ap.add_argument("--scenario", required=True); ap.add_argument("--timeout", type=float, default=460)
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
    def px(img, x, y):
        w, h, d = img; o = (y * w + x) * 3; return tuple(d[o:o + 3])
    def count(img, x0, y0, x1, y1, col): return sum(1 for y in range(y0, y1) for x in range(x0, x1) if px(img, x, y) == col)
    def region(img, x0, y0, x1, y1):
        w, h, d = img
        return b"".join(bytes(d[(y * w + x0) * 3:(y * w + x1) * 3]) for y in range(y0, y1))
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k): q.cmd("input-send-event", events=[key(k, True), key(k, False)]); time.sleep(0.2)
    def chord():   # Ctrl+Alt+Delete: modifiers down, Delete down/up, modifiers up
        q.cmd("input-send-event", events=[key("ctrl", True), key("alt", True), key("delete", True), key("delete", False), key("alt", False), key("ctrl", False)]); time.sleep(0.4)
    def move(x, y): q.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": raw_for(x, 1024)}}, {"type": "abs", "data": {"axis": "y", "value": raw_for(y, 768)}}])
    def btn(d): q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": "left"}}])
    def click(x, y): move(x, y); time.sleep(0.4); btn(True); btn(False); time.sleep(0.6)
    def focus_now(): return (re.findall(r"shell: focus=(\w+)", log()) or [None])[-1]
    def screen_now(): return (re.findall(r"shell: screen=(\w+)", log()) or [None])[-1]
    def opened(n): return wait_for(lambda: any(l.startswith("shell: review open ticket=%d requester=Aura (test)" % n) for l in lines("shell: review open")), 60)
    def states(): return [l[len("shell: review state="):] for l in lines("shell: review state=")]
    def closed_count(): return len(lines("shell: review closed"))
    STRIP = (208, 100, 816, 140)

    # ------------------------------------------------------------ review 1: Enter on the default focus must DECLINE
    check("review 1 opens (ticket 1, requester named by the kernel)", opened(1))
    time.sleep(0.8)
    img = shot()
    n_sec = count(img, *STRIP, SECURE)
    check("SCREEN: the compositor strip is drawn in the secure colour (%d pixels)" % n_sec, n_sec > 1500)
    open_img = img
    check("the DEFAULT FOCUS is Cancel (id 21)", focus_now() == "21")
    check("the guest's Settings/Home screen is untouched behind the modal", screen_now() in (None, "Home"))
    tap("ret")
    check("Enter on the default focus DECLINED the review (it never approves)", wait_for(lambda: "declined" in states(), 10) and not lines("shell: review approved"))
    check("the modal closed", wait_for(lambda: closed_count() >= 1, 10))
    time.sleep(0.5)
    closed_img = shot()
    check("SCREEN: after closing, no secure-colour pixel remains anywhere", count(closed_img, 0, 64, 1024, 768, SECURE) == 0)
    check("SCREEN: the scrim really covered the screen while it was open (a pixel outside the dialog differs from the same pixel afterwards)", px(open_img, 40, 400) != px(closed_img, 40, 400))

    # ------------------------------------------------------------ review 2: keys and clicks do not reach what is underneath; Escape declines
    check("review 2 opens (a NEW ticket, 2)", opened(2))
    time.sleep(0.8)
    before = shot()
    scr0 = screen_now()
    for k in ("h", "s", "a", "tab", "tab", "delete", "ctrl", "alt"):    # NOT Enter or Space: on the focused Cancel they would (rightly) decline. A lone Delete is no chord.
        tap(k)
    click(500, 700); click(40, 40); click(900, 300)
    after = shot()
    check("typing and clicking on the dock, header and scrim did not navigate anywhere behind it", screen_now() == scr0 and "approved" not in " ".join(states()))
    check("the review is still open and still OPEN (not armed by stray input)", closed_count() == 1 and states()[-1] == "open")
    check("Tab stayed inside the modal (focus is 21 only; Approve is locked)", focus_now() == "21")
    tap("esc")
    check("Escape declines", wait_for(lambda: states().count("declined") >= 2, 10) and wait_for(lambda: closed_count() >= 2, 10))

    # ------------------------------------------------------------ review 3: early gesture ignored, valid gesture arms, Tab then Enter approves; nothing changes
    check("review 3 opens (ticket 3)", opened(3))
    chord()                                          # sent within ~0.4 s of opening: the 600 ms arming delay has not passed
    time.sleep(0.2)
    check("a gesture sent at once after opening is IGNORED (still OPEN, Approve locked)", "armed" not in states() and states()[-1] == "open")
    tap("tab")
    check("Approve is locked: Tab cannot reach it", focus_now() == "21")
    time.sleep(1.2)
    chord()
    check("the valid gesture ARMS the review", wait_for(lambda: states()[-1:] == ["armed"], 10))
    time.sleep(0.5)
    check("ARMING DID NOT MOVE FOCUS: still on Cancel", focus_now() == "21")
    tap("tab")
    check("Tab now reaches Approve (id 22)", focus_now() == "22")
    tap("ret")
    check("Enter on Approve approves ticket 3 and the log says S1 execution is disabled", wait_for(lambda: any("ticket=3" in l and "S1 execution is disabled" in l for l in lines("shell: review approved")), 10))
    check("the modal closed and NO setting was touched", wait_for(lambda: closed_count() >= 3, 10) and "shell: setting row=" not in log())

    # ------------------------------------------------------------ review 4: left alone it EXPIRES after 60 s
    check("review 4 opens (ticket 4)", opened(4))
    t4 = time.time(); n0 = len(states())
    check("after 60 s the review expired and the modal says so (state expired, still open until Close)", wait_for(lambda: "expired" in states()[n0:], 90))
    waited = time.time() - t4
    check("it did not expire early (>= 58 s after it opened; %.0f s)" % waited, waited >= 58)
    time.sleep(0.5)
    check("SCREEN: the expired review is still drawn (strip visible) until Close", count(shot(), *STRIP, SECURE) > 1500)
    check("focus is on Close (id 21) and Approve is unreachable", focus_now() == "21")
    tap("ret")
    check("Close closes it without approving anything", wait_for(lambda: closed_count() >= 4, 10) and len(lines("shell: review approved")) == 1)
    time.sleep(0.5)
    check("SCREEN: afterwards no secure-colour pixel exists anywhere", count(shot(), 0, 64, 1024, 768, SECURE) == 0)
    chord()
    check("the chord with NO review open does nothing (no state change, no review, no crash)", len(lines("shell: review open")) == 4 and "KERNEL PANIC" not in log())
    check("the hostile and the demo UI clients were running the whole time", "shell: client c_ui1" in log() and "shell: client c_uibad" in log())
    print("review_check %s: %d passed, %d failed" % (a.scenario, passed, failed))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
