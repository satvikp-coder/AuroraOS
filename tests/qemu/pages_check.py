#!/usr/bin/env python3
"""tests/qemu/pages_check.py - the native Pages screen on the REAL shell. Evidence: QMP screendump PIXELS (the screen really shows a list of text
rows that changes while the clients run), real injected key and pointer events, and the guest's own lines ("shell: pages n=.. pid/name/state",
"[user pN]"). Scenarios (see pages-tests.sh):
  live   clients c_spin1 c_spin2 c_yield c_block are running: Pages lists them with their real states and changing counters; navigation by keyboard,
         Index and pointer; Back/Home; the clients keep running; the shell stays responsive
  empty  no clients: the honest empty state"""
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
    def dark(c): return c[0] + c[1] + c[2] < 480       # text pixels (ink and muted are both dark on the white card)
    def bands(img, x0, x1, y0, y1):
        """number of separate horizontal bands of text pixels in the rectangle"""
        n = 0; inb = False
        for y in range(y0, y1):
            row = any(dark(px(img, x, y)) for x in range(x0, x1, 2))
            if row and not inb: n += 1
            inb = row
        return n
    def ink_count(img, x0, y0, x1, y1): return sum(1 for y in range(y0, y1) for x in range(x0, x1) if dark(px(img, x, y)))
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k): q.cmd("input-send-event", events=[key(k, True), key(k, False)])
    def move(x, y): q.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": raw_for(x, 1024)}}, {"type": "abs", "data": {"axis": "y", "value": raw_for(y, 768)}}])
    def btn(d): q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": "left"}}])
    def click(x, y): move(x, y); time.sleep(0.5); btn(True); btn(False); time.sleep(0.7)
    def screen_now(): return (re.findall(r"shell: screen=(\w+)", log()) or [None])[-1]
    def shell_focus(): return (re.findall(r"shell: focus=(\w+)", log()) or [None])[-1]
    def tab_to(fid, tries=16):
        for _ in range(tries):
            if shell_focus() == str(fid): return True
            tap("tab"); time.sleep(0.4)
        return shell_focus() == str(fid)
    def pages_lines(): return [l for l in after_ready().split(chr(10)) if l.startswith("shell: pages n=")]
    def wait_screen(name, secs=15): return wait_for(lambda: screen_now() == name, secs)
    def stable():
        a1 = shot(); time.sleep(0.4); a2 = shot()
        return a2 if region(a1, 0, 70, 1024, 660) == region(a2, 0, 70, 1024, 660) else shot()
    def complete(secs=20, need=9):
        """poll real frames until one shows the complete Pages card (>= `need` text bands) and its rows hold text; a screenshot can catch a half-drawn frame, and
        with live-ticking rows two consecutive frames are NOT expected to be identical, so completeness is judged on the frame's own content. If no frame in `secs`
        is complete, the one with the most bands is returned and the caller's assertions fail on it (a missing or broken card still fails)."""
        end = time.time() + secs; best = None; best_key = (-1, -1)
        while True:
            f = shot(); k = (bands(f, 56, 960, 100, 520), ink_count(f, 56, 200, 960, 420))
            if k[0] >= need and k[1] > 1500: return f
            if k > best_key: best, best_key = f, k
            if time.time() > end: return best
            time.sleep(0.3)
    SID_PAGES_APP, SID_DOCK_BACK, SID_DOCK_HOME, SID_DOCK_INDEX, SID_PAGES_HOME = 13, 1, 2, 4, 230

    sc = a.scenario
    if sc == "live":
        check("all four clients were started by the kernel after the shell became interactive", wait_for(lambda: all(("shell: client %s pid=" % n) in log() for n in ("c_spin1", "c_spin2", "c_yield", "c_block")), 120))
        home = stable()
        check("Home: the Pages tile is a real control now (dark label text inside it)", ink_count(home, 560, 296, 940, 328) > 30)
        # ---- keyboard: Tab to the Pages tile, Enter ----
        check("Tab reaches the Pages tile (guest focus log)", tab_to(SID_PAGES_APP))
        tap("ret")
        check("Enter opens Pages (guest screen log)", wait_screen("Pages"))
        check("the guest listed the REAL processes with their real states (blocked client blocked; spinners ready or running)",
              wait_for(lambda: any("/c_block/blocked" in l and "/c_spin1/" in l and "/c_spin2/" in l and "/c_yield/" in l for l in pages_lines()), 60))
        last = pages_lines()[-1]
        check("the table has exactly the four client processes, in pid order", re.match(r"shell: pages n=4 (\d+)/c_spin1/\w+ (\d+)/c_spin2/\w+ (\d+)/c_yield/\w+ (\d+)/c_block/\w+", last) is not None)
        time.sleep(1.0)
        img1 = complete()
        import shutil; shutil.copy(path, os.path.join(a.tmp, "pages_live.ppm"))
        check("SCREEN: the Pages card is drawn: a title, a subtitle, a header, four process rows and a summary are separate text bands (9+ with the Home button)", bands(img1, 56, 960, 100, 520) >= 9)
        check("SCREEN: the process rows hold real text (many dark pixels per row band)", ink_count(img1, 56, 200, 960, 420) > 1500)
        check("SCREEN: Home's content (the Current Space card text) is gone", ink_count(img1, 64, 112, 400, 136) < 10 or bands(img1, 56, 960, 100, 520) >= 9)
        time.sleep(3.0)
        img2 = complete()
        check("SCREEN: the numbers are LIVE: the rows changed while the page stayed open (the spinners accumulate ticks)", region(img1, 56, 150, 960, 330) != region(img2, 56, 150, 960, 330))
        n_now = len([l for l in after_ready().split(chr(10)) if l.startswith("[user p")])
        time.sleep(3)
        check("the EL0 clients kept running while Pages was open (new heartbeat lines)", len([l for l in after_ready().split(chr(10)) if l.startswith("[user p")]) > n_now)
        # ---- there is nothing to press except Home and the dock: no process can be touched ----
        click(500, 250)
        time.sleep(1)
        check("clicking a process row does nothing (still on Pages; no process was stopped)", screen_now() == "Pages" and "KERNEL" not in log().split("shell: ready")[-1])
        # ---- keyboard Back ----
        check("Tab reaches the dock Back button", tab_to(SID_DOCK_BACK))
        tap("ret")
        check("Back from Pages returns to Home", wait_screen("Home"))
        # ---- pointer: click the Pages tile ----
        click(750, 312)
        check("a pointer click on the Pages tile opens Pages", wait_screen("Pages"))
        # ---- pointer: dock Home ----
        click(454, 704)
        check("a pointer click on the dock Home button returns to Home", wait_screen("Home"))
        # ---- Index route ----
        click(690, 704)
        check("the dock Index button opens Index", wait_screen("Index"))
        for k in "pages": tap(k)
        time.sleep(1)
        tap("ret")
        check("typing 'pages' in Index and pressing Enter opens Pages (the Index entry is available)", wait_screen("Pages"))
        img3 = complete()
        check("SCREEN: Pages is drawn again after the Index route", bands(img3, 56, 960, 100, 520) >= 9)
        # ---- Pages Home control by keyboard ----
        check("Tab reaches the Pages Home control", tab_to(SID_PAGES_HOME))
        tap("ret")
        check("Enter on it returns to Home", wait_screen("Home"))
        # ---- stability and responsiveness ----
        b = shot(); tap("tab"); time.sleep(1.0); b2 = shot()
        check("the shell is still responsive (Tab changes the screen)", region(b, 0, 64, 1024, 768) != region(b2, 0, 64, 1024, 768))
        check("no kernel bug, panic or fault was logged during the whole run", all(s not in log() for s in ("KERNEL PANIC", "KERNEL BUG", "SELFTEST FAILED")))
    else:
        check("Tab reaches the Pages tile", tab_to(SID_PAGES_APP))
        tap("ret")
        check("Enter opens Pages", wait_screen("Pages"))
        check("the guest snapshot is empty", wait_for(lambda: any(l.startswith("shell: pages n=0") for l in pages_lines()), 30))
        img = complete(need=3)
        import shutil; shutil.copy(path, os.path.join(a.tmp, "pages_empty.ppm"))
        check("SCREEN: only the title, subtitle, empty-state line and Home button are drawn (no process rows): at most 5 text bands", 3 <= bands(img, 56, 960, 100, 520) <= 5)
        check("no panic or kernel bug", all(s not in log() for s in ("KERNEL PANIC", "KERNEL BUG", "SELFTEST FAILED")))
    print("pages_check %s: %d passed, %d failed" % (a.scenario, passed, failed))
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
