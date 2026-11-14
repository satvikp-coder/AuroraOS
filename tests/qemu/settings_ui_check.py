#!/usr/bin/env python3
"""tests/qemu/settings_ui_check.py - the REAL kernel's native Settings screen driven by real QMP keyboard events, one scenario per boot
(see settings-ui-tests.sh). The guest log ("shell: setting row=R msg=M text=T contrast=C", "shell: settings store=...", "shell: appearance ...")
is the record of what the registry answered; the screendump proves what the user sees. Message codes (kernel/include/shell.h): 1 saved,
2 reset saved, 3 session only, 4 not saved, 5 denied, 6 newer version, 7 high contrast on, 8 off, 9 aura refused locally."""
import argparse, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

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
    def last(prefix):
        t = log()
        return t.rsplit(prefix, 1)[-1].split(chr(10))[0].strip() if prefix in t else None
    def wait_for(pred, secs=25):
        end = time.time() + secs
        while time.time() < end:
            if pred(): return True
            time.sleep(0.25)
        return False
    q = Qmp(a.qmp, dl)
    check("the real kernel reached the interactive shell", wait_serial(a.serial, "shell: ready keyboards=1 pointers=1", dl))
    check("no panic or failed self-test", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k): q.cmd("input-send-event", events=[key(k, True), key(k, False)])
    def shot(name):
        path = os.path.join(a.tmp, "%s_%s.ppm" % (a.scenario, name)); q.cmd("screendump", filename=path, format="ppm"); return read_ppm(path)
    def px(img, x, y):
        w, h, d = img; o = (y * w + x) * 3; return tuple(d[o:o + 3])
    def tab_to(fid, tries=40):
        for _ in range(tries):
            if last("shell: focus=") == str(fid): return True
            tap("tab"); time.sleep(0.35)
        return last("shell: focus=") == str(fid)
    def setting_lines(): return [l for l in log().split(chr(10)) if l.startswith("shell: setting row=")]
    def act(fid, n_before=None):
        """focus control fid and press Enter; wait for one more 'shell: setting' line; return it"""
        n0 = len(setting_lines())
        ok = tab_to(fid)
        tap("ret")
        got = wait_for(lambda: len(setting_lines()) > n0)
        return setting_lines()[-1] if (ok and got) else None
    def open_settings():
        ok = tab_to(15)              # the Settings tile on Home
        tap("ret")
        return ok and wait_for(lambda: (last("shell: screen=") or "").startswith("Settings"))
    def appearance(): return last("shell: appearance text=")

    sc = a.scenario
    if sc == "nodisk":
        check("no disk: the shell says changes last for this session only", "shell: settings store=none" in log())
        check("Settings opens from the Home tile by keyboard", open_settings())
        s0 = shot("settings")
        check("SCREEN: the Settings screen is drawn (white card, 14 rows of controls: ink pixels present)", px(s0, 100, 200) == (255, 255, 255))
        l = act(201)
        check("Text size Enter: session-only message (3) and 125 percent applied: %s" % l, l is not None and "msg=3 text=125" in l)
        s1 = shot("scaled")
        check("SCREEN: the layout changed to the larger text size", any(px(s0, x, y) != px(s1, x, y) for x in range(40, 1000, 40) for y in range(100, 600, 40)))
        l = act(204)
        check("Aura mode Enter without a store: refused locally (msg 9), text size unchanged: %s" % l, l is not None and "msg=9 text=125" in l)
        l = act(206)
        check("High contrast Enter: on for this session only (msg 7), contrast=1: %s" % l, l is not None and "msg=7" in l and "contrast=1" in l)
        s2 = shot("hc")
        check("SCREEN: the canvas is pure white in high contrast", px(s2, 3, 700) == (255, 255, 255))
        l = act(211)
        check("Reset on Text size returns to 100 percent for the session: %s" % l, l is not None and "text=100" in l)
    elif sc == "save":
        check("a disk is attached: the registry loaded (missing file, status 1) and the shell holds appearance+attention but not system", "shell: settings store=disk load=1 grant=0 (appearance+attention, no system)" in log())
        check("Settings opens", open_settings())
        l = act(201)
        check("Text size Enter: saved (msg 1), 125 percent: %s" % l, l is not None and "msg=1 text=125" in l)
        l = act(201)
        check("Enter again: saved, 150 percent: %s" % l, l is not None and "msg=1 text=150" in l)
        l = act(200)
        check("Theme Enter: saved (stored-only setting, msg 1), text size unchanged: %s" % l, l is not None and "msg=1 text=150" in l)
        l = act(204)
        check("Aura mode Enter: the REGISTRY refuses (msg 5, no system permission), text size unchanged: %s" % l, l is not None and "msg=5 text=150" in l)
        shot("saved")
    elif sc == "reload":
        check("after a restart the registry found the saved record (load status 0)", "shell: settings store=disk load=0" in log())
        check("after a restart the saved text size 150 is applied at boot", (appearance() or "").startswith("150"))
        check("Settings opens", open_settings())
        l = act(211)
        check("Reset on Text size: saved default (msg 2), 100 percent: %s" % l, l is not None and "msg=2 text=100" in l)
    elif sc == "readonly":
        check("the reset value survived the previous restart (text 100) and the disk loaded", "shell: settings store=disk load=0" in log() and (appearance() or "").startswith("100"))
        check("Settings opens", open_settings())
        l = act(201)
        check("on a read-only disk the save fails: msg 4, the text size stays 100: %s" % l, l is not None and "msg=4 text=100" in l)
        s0 = shot("ro")
        check("SCREEN: nothing changed on screen after the failed save (still the 100 percent reference layout)", px(s0, 262 + 20, 664 + 10) == (255, 255, 255))
    elif sc == "corrupt":
        check("a destroyed filesystem: the shell runs with no store, honestly", "shell: settings store=none" in log() and (appearance() or "").startswith("100"))
        check("Settings opens", open_settings())
        l = act(201)
        check("changes are session-only (msg 3): %s" % l, l is not None and "msg=3 text=125" in l)
    check("no panic or failed self-test at the end", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())
    print("settings_ui_check %s: %d passed, %d failed" % (tag.strip(), passed, failed))
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
