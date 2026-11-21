#!/usr/bin/env python3
"""tests/qemu/spaces_check.py - persistent Spaces (see spaces-tests.sh). Real QMP key events and the guest's own log lines: the shell prints
"shell: space restored=<Name>|none", "shell: space=<Name>", "shell: space saved=<Name>" / "shell: space NOT saved rc=N"."""
import argparse, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

SID_DOCK_INDEX = 4

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
    def boot_part():                       # everything before the shell is ready (restore/settings lines come first)
        t = log(); i = t.find("shell: ready keyboards=")
        return t[:i] if i >= 0 else t
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
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k): q.cmd("input-send-event", events=[key(k, True), key(k, False)]); time.sleep(0.25)
    def typ(t):
        for ch in t: tap(ch)
        time.sleep(0.4)
    def focus_now(): return (re.findall(r"shell: focus=(\w+)", log()) or [None])[-1]
    def screen_now(): return (re.findall(r"shell: screen=(\w+)", log()) or [None])[-1]
    def tab_to(fid, tries=24):
        for _ in range(tries):
            if focus_now() == str(fid): return True
            tap("tab"); time.sleep(0.3)
        return focus_now() == str(fid)
    def open_index():
        ok = tab_to(SID_DOCK_INDEX); tap("ret"); return ok and wait_for(lambda: screen_now() == "Index", 15)
    def pick_space(word, expect_save=True):   # Index -> type the Space's name -> Enter selects it
        ok = open_index(); typ(word); tap("ret")
        return ok and wait_for(lambda: any(l == "shell: space=" + word.capitalize() for l in lines("shell: space=")), 20)
    def saved(): return [l[len("shell: space saved="):] for l in lines("shell: space saved=")]
    def notsaved(): return lines("shell: space NOT saved")
    def restored(): return (re.findall(r"shell: space restored=(\w+)", boot_part()) or [None])[-1]
    sc = a.scenario

    if sc == "select":
        check("a fresh disk restores nothing: the default Space (Study) is used", restored() == "none")
        check("select Work from Index", pick_space("work"))
        check("the choice was SAVED through the settings registry (Work)", wait_for(lambda: saved()[-1:] == ["Work"], 20))
        check("select Projects", pick_space("projects"))
        check("saved (Projects)", wait_for(lambda: saved()[-1:] == ["Projects"], 20))
        check("select Personal", pick_space("personal"))
        check("saved (Personal)", wait_for(lambda: saved()[-1:] == ["Personal"], 20))
        check("select Work again: the final choice before the restart", pick_space("work"))
        check("saved (Work)", wait_for(lambda: saved()[-1:] == ["Work"], 20))
        check("nothing failed to save", not notsaved())
    elif sc == "restore":
        check("after a RESTART on the same disk the saved Space is restored before the first frame: Work", restored() == "Work")
        check("the shell starts on Home with Work selected (no navigation was needed)", screen_now() in (None, "Home"))
        check("Calculator is available (packaged disk)", open_index())
        typ("calc"); tap("ret")
        check("launch Calculator (a live process exists now)", wait_for(lambda: re.search(r"apps: launched calculator pid=\d+", log()) is not None, 30))
        check("select Projects while the Calculator runs", pick_space("projects"))
        check("saved (Projects): only the Space, nothing about the process", wait_for(lambda: saved()[-1:] == ["Projects"], 20))
    elif sc == "norun":
        check("after another restart Projects is restored", restored() == "Projects")
        check("the Calculator is NOT running: nothing was resumed, no launch happened in this boot", "apps: launched" not in log())
        check("Pages lists no calculator process", (open_index() and (typ("pages") or True) and (tap("ret") or True)) and wait_for(lambda: screen_now() == "Pages", 15) and wait_for(lambda: bool(lines("shell: pages n=")), 20) and not any("calculator" in l for l in lines("shell: pages n=")))
    elif sc == "rodisk":
        check("a read-only disk restores nothing", restored() == "none")
        check("select Work: the session switches", pick_space("work"))
        check("the failure to save is reported, never claimed as saved", wait_for(lambda: bool(notsaved()), 20) and "Work" not in saved())
    elif sc == "nodisk":
        check("no disk: the shell says settings are session-only", "settings store=none" in boot_part())
        check("select Work: the session switches", pick_space("work"))
        check("nothing is claimed saved and no save is attempted", not saved() and not notsaved())
    elif sc == "legacy":
        check("a record written before Spaces existed (40 bytes, text size 125) still loads: the text size is kept", "shell: appearance text=125" in boot_part())
        check("the record loaded fine but has no Space: the default (Study) applies, and it is not reported as a restored choice from a damaged file", restored() == "Study")
        check("select Projects", pick_space("projects"))
        check("saved; the registry rewrote the record in the new layout", wait_for(lambda: saved()[-1:] == ["Projects"], 20))
    elif sc == "legacy2":
        check("after a restart BOTH survive the layout change: text size 125", "shell: appearance text=125" in boot_part())
        check("and Projects is restored", restored() == "Projects")
    elif sc == "corrupt":
        check("a corrupt settings record falls back to defaults and says so (load=2)", "settings store=disk load=2" in boot_part())
        check("no Space is restored from a corrupt record", restored() == "none")
        check("select Work", pick_space("work"))
        check("the next save REPLACES the broken record", wait_for(lambda: saved()[-1:] == ["Work"], 20))
    elif sc == "badvalue":
        check("a record with a valid checksum but Space = 9 is corrupt: defaults", "settings store=disk load=2" in boot_part() and restored() == "none")
    check("no panic at the end", "KERNEL PANIC" not in log())
    print("spaces_check %s: %d passed, %d failed" % (sc, passed, failed))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
