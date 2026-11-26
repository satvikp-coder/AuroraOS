#!/usr/bin/env python3
"""tests/qemu/notes_check.py - the native Notes application (see notes-tests.sh). Real QMP key/pointer events, screendump pixels and the guest's own lines
(the EL0 process prints "notes ..." lines through SYS_WRITE; the kernel tags them "[user pN]")."""
import argparse, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

SID_DOCK_INDEX = 4
SENTENCE = "AuroraOS is my independent operating system."
KEYS = {" ": "spc", ".": "dot", ",": "comma", "-": "minus", "/": "slash", "\n": "ret"}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True); ap.add_argument("--tmp", required=True)
    ap.add_argument("--scenario", required=True); ap.add_argument("--timeout", type=float, default=600)
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
    def tap(k, mods=()):
        ev = [key(m, True) for m in mods] + [key(k, True), key(k, False)] + [key(m, False) for m in reversed(mods)]
        q.cmd("input-send-event", events=ev); time.sleep(0.25)
    def typ_shell(t):
        for ch in t: tap(ch)
        time.sleep(0.4)
    def type_note(t):
        for ch in t:
            if ch in KEYS: tap(KEYS[ch])
            elif ch.isupper(): tap(ch.lower(), ("shift",))
            else: tap(ch)
        time.sleep(0.4)
    def ctrl(k): tap(k, ("ctrl",)); time.sleep(0.3)
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
    def launched(): return re.findall(r"apps: launched notes pid=(\d+) principal=(\d+)", log())
    def surfaces(): return [(int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6))) for m in re.finditer(r"ui: surface slot=(\d+) pid=(\d+) at (\d+),(\d+) size (\d+)x(\d+)", log())]
    def since(n):                    # "[user pN] notes ..." payloads of the n-th launch only (a pid slot can be reused)
        t = after_ready(); marks = [m.start() for m in re.finditer(r"apps: launched notes", t)]
        if len(marks) < n: return []
        end = marks[n] if len(marks) > n else len(t)
        out = []
        for l in t[marks[n - 1]:end].split(chr(10)):
            m = re.match(r"\[user p\d+\] (notes .*)", l.rstrip(chr(13)))
            if m: out.append(m.group(1))
        return out
    def has(n, s): return any(l == s or l.startswith(s) for l in since(n))
    def last_text(n):
        t = [l for l in since(n) if l.startswith("notes text=")]
        return t[-1][len("notes text="):] if t else None
    def last_status(n):
        t = [l for l in since(n) if l.startswith("notes status=")]
        return t[-1][len("notes status="):] if t else None
    def launch(n, search="notes"):
        ok = open_index(); typ_shell(search); tap("ret")
        ok = ok and wait_for(lambda: len(launched()) == n, 30)
        return ok and wait_for(lambda: has(n, "notes started"), 30)
    def focus_surface(n):
        if len(launched()) < n: return None
        pid = int(launched()[n - 1][0]); s = [s for s in surfaces() if s[1] == pid][-1:]
        if not s: return None
        s = s[0]; click(s[2] + s[4] // 2, s[3] + s[5] // 2); return s
    def quit_notes(n):
        ctrl("q")
        pid = int(launched()[n - 1][0])
        return wait_for(lambda: any(("shell: reaped pid=%d exited code=0" % pid) in l for l in lines("shell: reaped")), 30)
    sc = a.scenario

    if sc == "first":
        check("boot registered Notes in the kernel registry (identity 602) with its UI capability", "apps: notes registered ui=granted" in log())
        check("Index finds Notes and Enter makes the KERNEL launch it under identity 602", launch(1) and launched()[0][1] == "602")
        check("a fresh disk: the app reports an empty note", has(1, "notes load empty") and last_text(1) == "|")
        check("the compositor placed its surface", wait_for(lambda: any(s[1] == int(launched()[0][0]) for s in surfaces()), 20))
        s = focus_surface(1)
        before = shot() if s else None
        type_note(SENTENCE)
        check("typing: the process saw every real key press, exactly " + repr(SENTENCE), wait_for(lambda: last_text(1) == SENTENCE + "|", 60))
        check("the status says there are unsaved changes before saving", last_status(1) == "Unsaved changes")
        check("SCREEN: the surface changed on the real display", s is not None and region(shot(), s[2], s[3], s[2] + s[4], s[3] + s[5]) != region(before, s[2], s[3], s[2] + s[4], s[3] + s[5]))
        ctrl("s")
        check("Ctrl+S saved it: written, READ BACK through AuroraFS and verified (seq 1)", wait_for(lambda: has(1, "notes save ok seq=1"), 20) and last_status(1) == "Saved")
        check("Ctrl+Q exits cleanly (exit code 0) and the process is reaped", quit_notes(1))
        check("the launcher's record is gone", wait_for(lambda: "apps: instance gone notes" in log(), 20))
        check("reopen Notes in the same session", launch(2))
        check("it loads the saved note: exactly the same text, from slot a", wait_for(lambda: has(2, "notes load ok slot=0"), 20) and last_text(2) == SENTENCE + "|")
        check("the status says Loaded", last_status(2) == "Loaded")
    elif sc == "second":
        check("after a RESTART of the emulator on the same disk, Notes launches", launch(1))
        check("it loads the exact saved text from AuroraFS", wait_for(lambda: has(1, "notes load ok slot=0"), 20) and last_text(1) == SENTENCE + "|")
        s = focus_surface(1)
        for _ in range(len("system.")): tap("backspace")
        check("backspace removed the last seven characters (shorter text)", wait_for(lambda: last_text(1) == "AuroraOS is my independent operating |", 30))
        ctrl("s")
        check("saving SHORTER text wrote the other slot (seq 2) and verified it", wait_for(lambda: has(1, "notes save ok seq=2"), 20))
        ctrl("r")
        check("Ctrl+R loads the previous verified save into the editor", wait_for(lambda:
              last_text(1) == SENTENCE + "|" and last_status(1) == "Previous save loaded. Ctrl+S to keep it", 20))
        check("exit discards the staged previous save", quit_notes(1) and has(1, "notes exit unsaved"))
    elif sc == "third":
        check("after a second restart Notes launches", launch(1))
        check("the SHORTER text is what loads (the newest slot wins; the stale longer slot is ignored)", wait_for(lambda: has(1, "notes load ok slot=1"), 20) and last_text(1) == "AuroraOS is my independent operating |")
        s = focus_surface(1)
        type_note("kernel and more words to make it longer than before. " * 1)
        edited = last_text(1)
        ctrl("r")
        check("Ctrl+R preserves unsaved edits", wait_for(lambda:
              last_status(1) == "Save edits before loading a previous save" and last_text(1) == edited, 20))
        ctrl("s")
        check("saving LONGER text works (seq 3, back in slot a)", wait_for(lambda: has(1, "notes save ok seq=3"), 25))
        check("exit", quit_notes(1))
    elif sc == "fourth":
        check("Notes launches", launch(1))
        check("the longer text loads after another restart", wait_for(lambda: has(1, "notes load ok slot=0"), 20) and (last_text(1) or "").startswith("AuroraOS is my independent operating kernel and more words"))
        check("no panic", "KERNEL PANIC" not in log())
    elif sc == "rodisk":
        check("Notes launches from a read-only disk", launch(1))
        check("it loads (empty)", has(1, "notes load empty"))
        focus_surface(1); type_note("abc"); ctrl("s")
        check("saving is refused HONESTLY: no 'Saved', the status names the read-only disk", wait_for(lambda: last_status(1) == "Disk is read-only", 20) and not has(1, "notes save ok"))
        check("the text stays in the editor, still unsaved", last_text(1) == "abc|")
        check("exit", quit_notes(1))
    elif sc == "nodisk":
        check("Index opens", open_index())
        typ_shell("notes"); tap("ret"); time.sleep(1.0)
        check("with no disk Notes is not available: nothing is launched", not launched() and screen_now() == "Index")
    elif sc == "corrupt":
        check("Notes launches", launch(1))
        check("a damaged saved note is NOT opened and NOT overwritten: it reports 'damaged' and locks", wait_for(lambda: has(1, "notes load damaged locked"), 20))
        focus_surface(1); type_note("abc"); ctrl("s")
        check("typing is ignored and Ctrl+S is refused while locked", wait_for(lambda: has(1, "notes save refused locked"), 20) and last_text(1) == "|" and not has(1, "notes save ok"))
        check("exit", quit_notes(1))
    elif sc in ("truncated", "short"):
        check("Notes launches", launch(1))
        check("a saved note that is cut off (%s) is NOT opened and NOT overwritten: it reports 'damaged' and locks" % sc, wait_for(lambda: has(1, "notes load damaged locked"), 20))
        focus_surface(1); type_note("abc"); ctrl("s")
        check("typing is ignored and Ctrl+S is refused while locked", wait_for(lambda: has(1, "notes save refused locked"), 20) and last_text(1) == "|" and not has(1, "notes save ok"))
        check("exit", quit_notes(1))
    elif sc == "full":
        check("Notes launches on a disk with every one of the 32 file slots used", launch(1))
        check("it starts with an empty note", wait_for(lambda: has(1, "notes load empty"), 20))
        focus_surface(1); type_note("abc"); ctrl("s")
        check("saving fails HONESTLY: the status says the disk is full and nothing is reported saved", wait_for(lambda: last_status(1) == "Disk or quota is full", 25) and not has(1, "notes save ok"))
        check("the text stays in the editor, still unsaved", last_text(1) == "abc|")
        check("exit", quit_notes(1))
    elif sc == "spacefull":
        check("Notes launches on a disk with ZERO free data blocks and loads the note that was already saved", launch(1) and wait_for(lambda: has(1, "notes load ok slot=0"), 20) and last_text(1) == "kept|")
        focus_surface(1); type_note("abc"); ctrl("s")
        check("saving fails HONESTLY: the status says the disk is full and nothing is reported saved", wait_for(lambda: last_status(1) == "Disk or quota is full", 25) and not has(1, "notes save ok"))
        check("the unsaved edit stays in the editor (nothing is silently dropped)", last_text(1) == "keptabc|")
        check("exit", quit_notes(1))
    elif sc == "spacefull2":
        check("after the failed save and a restart the PREVIOUS valid note is still there, unchanged", launch(1) and wait_for(lambda: has(1, "notes load ok slot=0"), 20) and last_text(1) == "kept|")
        focus_surface(1)
        check("exit", quit_notes(1))
    elif sc == "newer":
        check("Notes launches", launch(1))
        check("a note from a NEWER version is not opened and not overwritten", wait_for(lambda: has(1, "notes load newer version locked"), 20))
        focus_surface(1); ctrl("s")
        check("saving is refused", wait_for(lambda: has(1, "notes save refused locked"), 20))
        check("exit", quit_notes(1))
    elif sc == "foreign":
        check("Notes launches", launch(1))
        check("another principal's world-readable 'note.a' is invisible to Notes: it starts with an empty note", has(1, "notes load empty") and last_text(1) == "|")
        focus_surface(1); type_note("mine"); ctrl("s")
        check("it saves into its OWN files", wait_for(lambda: has(1, "notes save ok seq=1"), 25))
        check("exit", quit_notes(1))
    check("no panic at the end", "KERNEL PANIC" not in log())
    print("notes_check %s: %d passed, %d failed" % (sc, passed, failed))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
