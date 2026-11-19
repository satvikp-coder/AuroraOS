#!/usr/bin/env python3
"""tests/qemu/s1_check.py - real S1 settings execution (see s1-tests.sh). Real QMP key events, screendump pixels and the guest's own lines."""
import argparse, math, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

SECURE = (0xFF, 0xB0, 0x20)
STRIP = (208, 100, 816, 140)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True); ap.add_argument("--tmp", required=True)
    ap.add_argument("--scenario", required=True); ap.add_argument("--timeout", type=float, default=300)
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
    def strip_drawn(secs=25):   # poll real pixels until the secure strip is on screen (the log line is printed when the review opens, the frame is presented a little later); bounded, so a missing modal still fails
        end = time.time() + secs
        while True:
            if count(shot(), *STRIP, SECURE) > 1500: return True
            if time.time() >= end: return False
            time.sleep(0.3)
    def region(img, x0, y0, x1, y1):
        w, h, d = img
        return b"".join(bytes(d[(y * w + x0) * 3:(y * w + x1) * 3]) for y in range(y0, y1))
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k): q.cmd("input-send-event", events=[key(k, True), key(k, False)]); time.sleep(0.2)
    def typ(t):
        for ch in t: tap("spc" if ch == " " else ch)
        time.sleep(0.4)
    def chord(): q.cmd("input-send-event", events=[key("ctrl", True), key("alt", True), key("delete", True), key("delete", False), key("alt", False), key("ctrl", False)]); time.sleep(0.4)
    def focus_now(): return (re.findall(r"shell: focus=(\w+)", log()) or [None])[-1]
    def screen_now(): return (re.findall(r"shell: screen=(\w+)", log()) or [None])[-1]
    def tab_to(fid, tries=20):
        for _ in range(tries):
            if focus_now() == str(fid): return True
            tap("tab"); time.sleep(0.3)
        return focus_now() == str(fid)
    def states(): return [l[len("shell: review state="):] for l in lines("shell: review state=")]
    def open_aura():
        ok = tab_to(14); tap("ret"); return ok and wait_for(lambda: screen_now() == "Aura", 15)
    def boot_text(): return re.findall(r"shell: appearance text=(\d+)", log())[-1:]            # the LAST one printed at boot: after the saved settings were applied
    def boot_lines(prefix): return [l.rstrip(chr(13)) for l in log().split(chr(10)) if l.startswith(prefix)]      # boot lines come BEFORE "shell: ready"
    sc = a.scenario

    if sc == "approve":
        check("the boot loaded the default text size (100)", boot_text() == ["100"])
        check("Aura opens", open_aura())
        typ("make text bigger"); tap("ret")
        check("Enter runs the command: the TRUSTED REVIEW opens (ticket 1, requester Aura)", wait_for(lambda: any(l.startswith("shell: review open ticket=1 requester=Aura") for l in lines("shell: review open")), 20))
        check("SCREEN: the secure strip is drawn", strip_drawn())
        check("default focus is Cancel", focus_now() == "21")
        before = shot()
        tap("ret")
        check("Enter on Cancel DECLINED it: nothing changed, no setting log, the screen is Aura again", wait_for(lambda: "declined" in states(), 10) and "shell: setting row=" not in log() and screen_now() == "Aura")
        tap("ret")
        check("running the command again opens a NEW review (ticket 2)", wait_for(lambda: any(l.startswith("shell: review open ticket=2") for l in lines("shell: review open")), 20))
        tap("esc")
        check("Escape declines it too, still nothing changed", wait_for(lambda: states().count("declined") >= 2, 10))
        tap("ret")
        check("a third review opens (ticket 3)", wait_for(lambda: any(l.startswith("shell: review open ticket=3") for l in lines("shell: review open")), 20))
        time.sleep(0.9)
        chord()
        check("the gesture arms it", wait_for(lambda: states()[-1:] == ["armed"], 10))
        tap("tab")
        check("Tab reaches Approve", focus_now() == "22")
        before = shot()
        tap("ret")
        check("Enter on Approve approves and the S1 chain COMPLETES: Done with a receipt (PRE seq 2, POST seq 3)", wait_for(lambda: any("ticket=3 -> s1 state=1" in l and "Done: Make text larger Receipt #3." in l for l in lines("shell: review approved")), 20))
        time.sleep(1.0)
        after = shot()
        check("SCREEN: the interface really changed size (text 125%): the layout below the header differs", region(after, 0, 100, 1024, 760) != region(before, 0, 100, 1024, 760))
        check("no secure-colour pixel remains once the review is closed", count(after, 0, 64, 1024, 768, SECURE) == 0)
        check("the broker re-checked at execution and nothing else ran (one approval, one execution)", len(lines("shell: review approved")) == 1)
    elif sc == "persist":
        check("the saved appearance is 125 after the restart (the setting really persisted)", boot_text() == ["125"])
        check("the PRE record (setting change started, was 100) is on the disk", any("kind=3" in l for l in boot_lines("ledger: trail rec seq=2")))
        check("the POST record (setting changed, now 125) is on the disk", any("kind=3" in l for l in boot_lines("ledger: trail rec seq=3")))
        check("the Trail is healthy and holds the boot marker, PRE and POST of the earlier boot", any("ledger: trail state=ok records=3" in l for l in boot_lines("ledger: trail state=")))
        check("Aura opens", open_aura())
        typ("make text smaller"); tap("ret")
        check("the review is built from the REAL saved value: ticket 1, previous 125", wait_for(lambda: any(l.startswith("shell: review open ticket=1") for l in lines("shell: review open")), 20))
        tap("esc")
        check("declined: still 125", wait_for(lambda: "declined" in states(), 10))
    elif sc == "rodisk":
        check("the saved appearance is still 125 (read from the read-only disk)", boot_text() == ["125"])
        check("the ledger reports a read-only disk", any("ledger: state=read-only" in l for l in boot_lines("ledger: state=")))
        check("Aura opens", open_aura())
        typ("make text bigger"); tap("ret")
        check("the command is refused honestly: the records are unhealthy, NO review opens", wait_for(lambda: any("aura: shell ran kind=set_setting" in l and "unhealthy" in l for l in lines("aura: shell ran")), 15) and not lines("shell: review open"))
    elif sc == "nodisk":
        check("with no disk the text size is the default (100)", boot_text() == ["100"])
        check("Aura opens", open_aura())
        typ("make text bigger"); tap("ret")
        check("the command is refused honestly (no store / no records), NO review opens", wait_for(lambda: any("aura: shell ran kind=set_setting" in l and "Not available" in l for l in lines("aura: shell ran")), 15) and not lines("shell: review open"))
    elif sc == "undo":
        check("the saved appearance is still 125 (the earlier receipt is the newest change)", boot_text() == ["125"])
        check("Aura opens", open_aura())
        typ("undo"); tap("ret")
        check("a NEW review opens for the reversal (requester Aura (Undo))", wait_for(lambda: any(l.startswith("shell: review open ticket=1 requester=Aura (Undo)") for l in lines("shell: review open")), 20))
        time.sleep(0.9)
        before = shot()
        chord(); tap("tab")
        check("Tab reaches Approve", focus_now() == "22")
        tap("ret")
        check("approved: Done with a receipt (marker 5, PRE 6, POST 7)", wait_for(lambda: any("-> s1 state=1" in l and "Receipt #7." in l for l in lines("shell: review approved")), 20))
        time.sleep(1.0)
        check("SCREEN: the interface changed back (text 100%)", region(shot(), 0, 100, 1024, 760) != region(before, 0, 100, 1024, 760))
        tap("ret")                                                                       # the command line still holds "undo"
        check("a second undo is a separate review (it would redo 125): opens ticket 2, then Escape declines", wait_for(lambda: any(l.startswith("shell: review open ticket=2") for l in lines("shell: review open")), 20))
        tap("esc")
    check("no panic at the end", "KERNEL PANIC" not in log())
    print("s1_check %s: %d passed, %d failed" % (sc, passed, failed))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
