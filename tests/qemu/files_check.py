#!/usr/bin/env python3
"""tests/qemu/files_check.py - the native Files viewer (see files-tests.sh). Real QMP key/pointer events, screendump pixels and the guest's own lines
(the EL0 process prints "files ..." lines through SYS_WRITE; the kernel tags them "[user pN]")."""
import argparse, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

SID_DOCK_INDEX = 4

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True); ap.add_argument("--tmp", required=True)
    ap.add_argument("--scenario", required=True); ap.add_argument("--timeout", type=float, default=400)
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
    def tap(k): q.cmd("input-send-event", events=[key(k, True), key(k, False)]); time.sleep(0.3)
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
    def open_index():
        ok = tab_to(SID_DOCK_INDEX); tap("ret"); return ok and wait_for(lambda: screen_now() == "Index", 15)
    def launched(): return re.findall(r"apps: launched files pid=(\d+) principal=(\d+)", log())
    def surfaces(): return [(int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6))) for m in re.finditer(r"ui: surface slot=(\d+) pid=(\d+) at (\d+),(\d+) size (\d+)x(\d+)", log())]
    def since(n):
        t = after_ready(); marks = [m.start() for m in re.finditer(r"apps: launched files", t)]
        if len(marks) < n: return []
        end = marks[n] if len(marks) > n else len(t)
        out = []
        for l in t[marks[n - 1]:end].split(chr(10)):
            m = re.match(r"\[user p\d+\] (files .*)", l.rstrip(chr(13)))
            if m: out.append(m.group(1))
        return out
    def has(n, s): return any(l.startswith(s) for l in since(n))
    def rows(n):
        out = []; started = False
        for l in since(n):
            if l.startswith("files list n="): out = []; started = True
            elif l.startswith("files row=") and started: out.append(l[len("files row="):])
        return out
    def sels(n): return [l[len("files sel="):] for l in since(n) if l.startswith("files sel=")]
    def launch(n):
        ok = open_index(); typ("files"); tap("ret")
        ok = ok and wait_for(lambda: len(launched()) == n, 30)
        return ok and wait_for(lambda: has(n, "files started"), 30)
    def focus_surface(n):
        if len(launched()) < n: return None
        pid = int(launched()[n - 1][0]); s = [s for s in surfaces() if s[1] == pid][-1:]
        if not s: return None
        s = s[0]; click(s[2] + s[4] // 2, s[3] + s[5] // 2); return s
    def quit_files(n):
        tap("q"); pid = int(launched()[n - 1][0])
        return wait_for(lambda: any(("shell: reaped pid=%d exited code=0" % pid) in l for l in lines("shell: reaped")), 30)
    sc = a.scenario

    if sc == "list":
        check("boot registered Files (identity 603) with its UI capability", "apps: files registered ui=granted" in log())
        check("Index finds Files and Enter makes the KERNEL launch it under identity 603", launch(1) and launched()[0][1] == "603")
        check("the list was produced by the kernel's permission-checked LIST: exactly the 7 readable files", wait_for(lambda: has(1, "files list n=7"), 20) and len(rows(1)) == 7)
        r = rows(1)
        names = [x.split()[0] for x in r]
        check("the names are calc.elf notes.elf files.elf readme.txt blob.bin empty.txt and the long name", names[:6] == ["calc.elf", "notes.elf", "files.elf", "readme.txt", "blob.bin", "empty.txt"] and names[6].startswith("abcdefghijklmnopqr"))
        check("a private file of another principal (secret.txt) is invisible: not listed anywhere", not any("secret" in l for l in since(1)))
        check("Notes' private note.a (principal 602) is invisible too", not any("note.a" in l for l in since(1)))
        check("owners are shown in words: installer files 'system', others 'other'", all(x.endswith("system") for x in r[:3]) and all(x.endswith("other") for x in r[3:]))
        check("the first row is selected and previewed as a binary executable (no byte shown)", sels(1)[:1] == ["calc.elf"] and has(1, "files preview binary"))
        s = focus_surface(1)
        before = shot() if s else None
        tap("down"); tap("down"); tap("down")
        check("Down x3 selects readme.txt and previews its exact text", wait_for(lambda: sels(1)[-1:] == ["readme.txt"], 20) and wait_for(lambda: has(1, "files preview line=Hello from the Files viewer test."), 20))
        check("the second line of the file is shown on its own line", has(1, "files preview line=Second line"))
        check("SCREEN: the surface changed on the real display", s is not None and region(shot(), s[2], s[3], s[2] + s[4], s[3] + s[5]) != region(before, s[2], s[3], s[2] + s[4], s[3] + s[5]))
        tap("down"); check("blob.bin is binary: no preview content", wait_for(lambda: sels(1)[-1:] == ["blob.bin"], 20) and wait_for(lambda: since(1)[-3:].count("files preview binary") >= 1, 20))
        tap("down"); check("empty.txt is reported empty", wait_for(lambda: sels(1)[-1:] == ["empty.txt"], 20) and wait_for(lambda: has(1, "files preview empty"), 20))
        for _ in range(5): tap("down")
        check("Down at the bottom stays on the last row", sels(1)[-1].startswith("abcdefghijklmnopqr"))
        for _ in range(10): tap("up")
        check("Up at the top stays on the first row", sels(1)[-1] == "calc.elf")
        tap("r"); check("R refreshes the list from the kernel (same 7 files)", wait_for(lambda: len([l for l in since(1) if l.startswith("files list n=")]) >= 2, 20))
        check("Q exits cleanly (code 0) and the process is reaped", quit_files(1))
        check("the launcher's record is gone", wait_for(lambda: "apps: instance gone files" in log(), 20))
        check("relaunch is a fresh instance with the same identity", launch(2) and launched()[1][1] == "603" and wait_for(lambda: has(2, "files list n=7"), 20))
        check("the fresh instance starts at the first row", sels(2)[:1] == ["calc.elf"])
        check("exit", quit_files(2))
    elif sc == "many":
        check("Files launches", launch(1))
        check("only the first 16 of 21 readable files are shown, and the view says so", wait_for(lambda: has(1, "files list n=16"), 20) and wait_for(lambda: has(1, "files status=Showing the first 16 files"), 20))
        s = focus_surface(1)
        for _ in range(20): tap("down")
        check("scrolling to the bottom selects the 16th entry and never beyond", wait_for(lambda: len(sels(1)) >= 16, 30) and len(set(sels(1))) <= 16)
        check("exit", quit_files(1))
    elif sc == "nodisk":
        check("Index opens", open_index())
        typ("files"); tap("ret"); time.sleep(1.0)
        check("with no disk Files is not available: nothing is launched", not launched() and screen_now() == "Index")
    check("no panic at the end", "KERNEL PANIC" not in log())
    print("files_check %s: %d passed, %d failed" % (sc, passed, failed))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
