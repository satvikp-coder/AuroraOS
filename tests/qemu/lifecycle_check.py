#!/usr/bin/env python3
"""tests/qemu/lifecycle_check.py - several genuine EL0 applications at once (see lifecycle-tests.sh): Calculator, Notes and Files launched from Index under their
kernel-chosen identities, listed in Pages, isolated from one another, one exits while the others keep running, and a relaunch is a fresh instance.
Real QMP key/pointer events and the guest's own log lines (each program prints tagged "[user pN] ..." lines)."""
import argparse, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, wait_serial

SID_DOCK_INDEX = 4
APPS = (("calculator", "calc", 601), ("notes", "notes", 602), ("files", "files", 603))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True); ap.add_argument("--tmp", required=True)
    ap.add_argument("--timeout", type=float, default=600)
    a = ap.parse_args()
    passed = failed = 0
    dl = time.time() + a.timeout
    def check(name, ok):
        nonlocal passed, failed
        if ok: passed += 1; print("  PASS: [lifecycle] " + name)
        else: failed += 1; print("  FAIL: [lifecycle] " + name)
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
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k, mods=()):
        q.cmd("input-send-event", events=[key(m, True) for m in mods] + [key(k, True), key(k, False)] + [key(m, False) for m in reversed(mods)]); time.sleep(0.25)
    def typ(t):
        for ch in t: tap(ch)
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
    def launches(key): return re.findall(r"apps: launched %s pid=(\d+) principal=(\d+)" % key, log())
    def surfaces(pid): return [(int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6))) for m in re.finditer(r"ui: surface slot=(\d+) pid=(%d) at (\d+),(\d+) size (\d+)x(\d+)" % pid, log())]
    def user(pid, since_marker=0): return [l[len("[user p%d] " % pid):] for l in after_ready()[since_marker:].split(chr(10)) if l.startswith("[user p%d] " % pid)]
    def launch(key, word, n):
        ok = tab_to(SID_DOCK_INDEX); tap("ret"); ok = ok and wait_for(lambda: screen_now() == "Index", 15)
        typ(word); tap("ret")
        return ok and wait_for(lambda: len(launches(key)) == n, 30)
    def focus(pid):
        s = surfaces(pid)
        if not s: return False
        x, y, w, h = s[-1]; click(x + w // 2, y + h // 2); return True
    def pages_has(pid, name): return any(("%d/%s/" % (pid, name)) in l for l in lines("shell: pages n=")[-1:])
    def reaped(pid): return any(("shell: reaped pid=%d exited code=0" % pid) in l for l in lines("shell: reaped"))

    pids = {}
    for i, (key_, word, prin) in enumerate(APPS):
        check("launch %s from Index: the kernel assigns identity %d" % (key_, prin), launch(key_, word, 1) and launches(key_)[0][1] == str(prin))
        pids[key_] = int(launches(key_)[0][0]) if launches(key_) else 0
    check("three distinct live processes", len(set(pids.values())) == 3 and 0 not in pids.values())
    check("each program started and drew its first state", wait_for(lambda: any("calc display=0" in l for l in user(pids["calculator"])) and any("notes started" in l for l in user(pids["notes"])) and any("files started" in l for l in user(pids["files"])), 40))
    check("each has its own compositor-placed surface", all(surfaces(p) for p in pids.values()))
    tab_to(13); tap("ret")           # Pages tile on Home may not be focused from an app screen: fall back to the dock/Index route
    ok_pages = wait_for(lambda: screen_now() == "Pages", 5)
    if not ok_pages:
        tab_to(SID_DOCK_INDEX); tap("ret"); wait_for(lambda: screen_now() == "Index", 15); typ("pages"); tap("ret"); ok_pages = wait_for(lambda: screen_now() == "Pages", 15)
    check("Pages lists all three running apps by name", ok_pages and wait_for(lambda: all(pages_has(pids[k], k) for k in pids), 30))
    check("Files sees only what it may read: not Notes' private files, not the settings or ledger files",
          wait_for(lambda: any(l.startswith("files list n=") for l in user(pids["files"])), 20) and not any("note.a" in l or "settings" in l or "ledger" in l for l in user(pids["files"])))

    # input goes only to the focused app
    nlines0 = len(user(pids["calculator"]))
    ok = focus(pids["notes"]); typ("hi")
    check("typing into the focused Notes reaches Notes only", ok and wait_for(lambda: any(l == "notes text=hi|" for l in user(pids["notes"])), 20))
    check("the Calculator received none of those keys", len(user(pids["calculator"])) == nlines0)
    ok = focus(pids["calculator"]); tap("1"); tap("2"); tap("kp_add"); tap("8"); tap("kp_enter")
    check("then 12 + 8 = 20 in the Calculator", ok and wait_for(lambda: any("calc display=20" in l for l in user(pids["calculator"])), 20))
    check("Notes still shows only its own text (no digits leaked into it)", [l for l in user(pids["notes"]) if l.startswith("notes text=")][-1] == "notes text=hi|")

    # one exits, the others keep running
    focus(pids["calculator"]); tap("q")
    check("Q exits the Calculator cleanly (code 0) and it is reaped", wait_for(lambda: reaped(pids["calculator"]), 30))
    check("only the Calculator's launcher record and surface are gone", wait_for(lambda: "apps: instance gone calculator" in log() and "apps: instance gone notes" not in log() and "apps: instance gone files" not in log(), 20))
    focus(pids["notes"]); typ("yo")
    check("Notes still works after the Calculator exited", wait_for(lambda: [l for l in user(pids["notes"]) if l.startswith("notes text=")][-1] == "notes text=hiyo|", 20))
    check("Files is still alive: its process was not reaped", not reaped(pids["files"]))

    # a fresh Calculator instance
    marker = len(after_ready())
    check("relaunch the Calculator", launch("calculator", "calc", 2) and launches("calculator")[1][1] == "601")
    p2 = int(launches("calculator")[1][0]) if len(launches("calculator")) > 1 else 0
    check("the new instance starts at 0 with no stale state from the old one", wait_for(lambda: any("calc display=0" in l for l in user(p2, marker)), 30) and not any("calc display=20" in l for l in user(p2, marker)))
    check("Notes' text is unchanged by the relaunch (separate address spaces and files)", [l for l in user(pids["notes"]) if l.startswith("notes text=")][-1] == "notes text=hiyo|")

    # quit the others cleanly
    focus(pids["notes"]); tap("q", ("ctrl",))
    check("Ctrl+Q exits Notes (unsaved: it says so and nothing was written)", wait_for(lambda: reaped(pids["notes"]), 30) and any(l == "notes exit unsaved" for l in user(pids["notes"])))
    focus(pids["files"]); tap("q")
    check("Q exits Files", wait_for(lambda: reaped(pids["files"]), 30))
    check("no panic or fault was logged during the whole run", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log() and not lines("shell: faulted"))
    print("lifecycle_check: %d passed, %d failed" % (passed, failed))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
