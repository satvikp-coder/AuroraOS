#!/usr/bin/env python3
"""tests/qemu/home_check.py - the REAL kernel (no test flags) booted with ramfb + virtio keyboard + tablet: native Aurora Glass Home,
Dock, Spaces and the About modal, driven by real QMP events and verified on QEMU screendump pixels and the guest's "shell:" log.
Layout constants below come from DESIGN.md tokens and the layout written in kernel/ui/shell.c, restated here independently.
Time-dependent pixels (the uptime label at x>=700,y<64) are never compared exactly."""
import argparse, math, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

def rgb(v): return ((v >> 16) & 255, (v >> 8) & 255, v & 255)
CANVAS, SURFACE, INK, MUTED, ACCENT, FOCUS, SCRIM, AURA = map(rgb, (0xEEF3F8, 0xFFFFFF, 0x142136, 0x52627A, 0x3478F6, 0x175CD3, 0x525A66, 0x00AEB8))
AXMAX = 32767
def raw_for(p, extent): return math.ceil(p * AXMAX / (extent - 1))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True); ap.add_argument("--tmp", required=True)
    ap.add_argument("--timeout", type=float, default=400)
    a = ap.parse_args()
    passed = failed = 0
    dl = time.time() + a.timeout
    def check(name, ok):
        nonlocal passed, failed
        if ok: passed += 1; print("  PASS: " + name)
        else: failed += 1; print("  FAIL: " + name)
    def log():
        with open(a.serial, "rb") as f: return f.read().decode("latin-1")
    def count(s): return log().count(s)
    def wait_count(s, n, secs=20):
        end = time.time() + secs
        while time.time() < end:
            if count(s) >= n: return True
            time.sleep(0.2)
        return False

    q = Qmp(a.qmp, dl)
    check("the real kernel reached the interactive shell", wait_serial(a.serial, "shell: ready keyboards=1 pointers=1", dl))
    check("Home was drawn at display bring-up (before the self-tests)", log().index("shell: home drawn") < log().index("Boot complete"))
    check("no panic or failed self-test", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())

    def shot(name):
        path = os.path.join(a.tmp, name + ".ppm"); q.cmd("screendump", filename=path, format="ppm"); return read_ppm(path)
    def px(img, x, y):
        w, h, d = img; o = (y * w + x) * 3; return tuple(d[o:o + 3])
    def until(name, pred, secs=15):
        end = time.time() + secs
        while True:
            img = shot(name)
            if pred(img) or time.time() > end: return img
            time.sleep(0.3)
    def n_color(img, x0, y0, x1, y1, col):
        return sum(1 for y in range(y0, y1) for x in range(x0, x1) if px(img, x, y) == col)
    def changed(i1, i2, x0, y0, x1, y1): return any(px(i1, x, y) != px(i2, x, y) for y in range(y0, y1) for x in range(x0, x1))
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k, *mods): q.cmd("input-send-event", events=[key(m, True) for m in mods] + [key(k, True), key(k, False)] + [key(m, False) for m in reversed(mods)])
    def move(x, y): q.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": raw_for(x, 1024)}}, {"type": "abs", "data": {"axis": "y", "value": raw_for(y, 768)}}])
    def btn(d): q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": "left"}}])
    def cursor_at(x, y): return lambda i: px(i, x - 6, y) == INK and px(i, x, y) == SURFACE
    def click_at(x, y, name):
        move(x, y); until(name, cursor_at(x, y)); btn(True); btn(False)
    def acts(): return count("shell: activate ")
    def last(prefix):
        t = log()
        return t.rsplit(prefix, 1)[-1].split(chr(10))[0].strip() if prefix in t else None
    def wait_last(prefix, value, secs=20):
        end = time.time() + secs
        while time.time() < end:
            if last(prefix) == value: return True
            time.sleep(0.2)
        return False
    def tab_to(fid, shift=False, tries=14):
        for _ in range(tries):
            if last("shell: focus=") == str(fid): return True
            tap("tab", *(("shift",) if shift else ())); time.sleep(0.35)
        return last("shell: focus=") == str(fid)

    # ---------------- Home ----------------
    h0 = until("home0", lambda i: px(i, 500, 65) == AURA)
    check("Home: white header, aura-coloured Horizon rail, canvas background", px(h0, 500, 30) == SURFACE and px(h0, 500, 65) == AURA and px(h0, 500, 640) == CANVAS)
    check("Home: Current Space, System and Applications cards are white", px(h0, 300, 150) == SURFACE and px(h0, 300, 600) == SURFACE and px(h0, 700, 560) == SURFACE)
    check("Home: header title 'AuroraOS' and the Study label are drawn (ink pixels)", n_color(h0, 24, 14, 200, 50, INK) > 50 and n_color(h0, 64, 142, 300, 182, INK) > 50)
    check("Home: the uptime label is drawn (muted ink pixels in the header right)", n_color(h0, 760, 20, 1000, 48, MUTED) > 10)
    check("Home: the Home dock item is active (accent), the others are not", px(h0, 410, 704) == ACCENT and px(h0, 520, 704) != ACCENT and px(h0, 640, 704) != ACCENT)
    check("Home: Spaces and Index tiles use ink text, the unavailable tiles use muted text only", n_color(h0, 544, 148, 960, 272, INK) > 30 and n_color(h0, 544, 284, 960, 476, INK) == 0 and n_color(h0, 544, 284, 960, 476, MUTED) > 30)
    check("Home: Back is drawn muted (disabled) with no history", n_color(h0, 280, 680, 392, 728, MUTED) > 10 and n_color(h0, 280, 680, 392, 728, INK) == 0)
    check("Home: the pointer cursor is at the screen centre", cursor_at(512, 384)(h0) or px(h0, 506, 384) == INK)
    check("Home: no focus ring before any key is pressed", n_color(h0, 0, 64, 1024, 768, FOCUS) == 0)

    # ---------------- keyboard focus ----------------
    tap("tab")
    check("Tab: focus lands on Change Space (id 10)", wait_last("shell: focus=", "10"))
    h1 = until("home_tab1", lambda i: px(i, 164, 229) == FOCUS)
    check("SCREEN: a 3 px focus ring surrounds Change Space", all(px(h1, 164, 229 + k) == FOCUS for k in range(3)) and px(h1, 164, 228) != FOCUS)
    seen = []
    for want in ("16", "11", "12", "2", "3", "4", "10"):
        tap("tab"); time.sleep(0.4); ok_ = wait_last("shell: focus=", want, 5); seen.append(ok_)
    check("Tab order: About, Spaces tile, Index tile, dock Home, Spaces, Index, then wraps to Change Space; disabled tiles and Back are skipped", all(seen))
    check("Tab never focused a disabled control (Pages 13, Aura 14, Settings 15, Back 1)", all(("shell: focus=%d" % i + chr(10)) not in log() for i in (13, 14, 15, 1)))
    tap("tab", "shift"); time.sleep(0.4)
    check("Shift+Tab wraps backwards to the Index dock item (4)", wait_last("shell: focus=", "4", 5))
    check("Shift+Tab can walk back to the Spaces tile (11)", tab_to(11, shift=True))

    # ---------------- activation: open Spaces by keyboard ----------------
    n = acts()
    tap("ret")
    check("Enter on the Spaces tile opens Spaces (history 1)", wait_last("shell: screen=", "Spaces history=1") and acts() == n + 1)
    s1 = until("spaces", lambda i: px(i, 520, 704) == ACCENT)
    check("SCREEN: the Spaces dock item is active; Home is no longer", px(s1, 520, 704) == ACCENT and px(s1, 410, 704) != ACCENT)
    check("SCREEN: four Space buttons are listed; Study is the active one (accent)", px(s1, 100, 224) == ACCENT and px(s1, 100, 296) != ACCENT and px(s1, 100, 368) != ACCENT and px(s1, 100, 440) != ACCENT)
    check("SCREEN: Back is enabled now (ink text)", n_color(s1, 280, 680, 392, 728, INK) > 10)

    # ---------------- pointer: choose a Space ----------------
    click_at(260, 296, "ptr_work")
    check("pointer click on Work selects it", wait_count("shell: space=Work", 1))
    s2 = until("space_work", lambda i: px(i, 100, 296) == ACCENT)
    check("SCREEN: Work is active, Study is not", px(s2, 100, 296) == ACCENT and px(s2, 100, 224) != ACCENT)
    # Dock Home by pointer
    click_at(454, 704, "ptr_home")
    check("dock Home returns to Home (history 2)", wait_last("shell: screen=", "Home history=2"))
    h2 = until("home_work", lambda i: px(i, 410, 704) == ACCENT)
    check("SCREEN: Home now shows 'Work' in the Current Space card (text changed from 'Study')", changed(h0, h2, 64, 142, 300, 182) and px(h2, 410, 704) == ACCENT)
    # Back via the dock
    click_at(336, 704, "ptr_back")
    check("dock Back returns to the previous screen (Spaces, history 1)", wait_last("shell: screen=", "Spaces history=1"))
    click_at(454, 704, "ptr_home2")
    check("dock Home again", wait_last("shell: screen=", "Home history=2"))

    # ---------------- disabled controls ----------------
    until("ptr_idle", lambda i: True)
    base = acts()
    click_at(752, 312, "ptr_pages"); click_at(752, 380, "ptr_aura"); click_at(752, 448, "ptr_settings")
    time.sleep(1.0)
    check("clicks on the unavailable Pages, Aura and Settings tiles activate nothing", acts() == base and last("shell: screen=").startswith("Home"))

    # ---------------- modal ----------------
    check("keyboard focus can reach the About button (id 16)", tab_to(16))
    tap("ret")
    check("Enter on About opens the modal", wait_count("shell: modal open", 1))
    m1 = until("modal", lambda i: px(i, 600, 650) == SCRIM)
    check("SCREEN: scrim covers the content and the dock; the dialog is white with a focus-coloured border", px(m1, 600, 650) == SCRIM and px(m1, 100, 720) == SCRIM and px(m1, 512, 300) == SURFACE and px(m1, 262, 350) == FOCUS)
    check("SCREEN: the Close button has the focus ring", all(px(m1, 512, 401 + k) == FOCUS for k in range(3)) or all(px(m1, 512, 400 + k) == FOCUS for k in range(3)))
    base = acts()
    for _ in range(3): tap("tab"); time.sleep(0.3)
    check("Tab inside the dialog keeps focus on Close (id 20)", last("shell: focus=") == "20")
    click_at(752, 176, "ptr_cov1"); click_at(572, 704, "ptr_cov2")
    time.sleep(1.0)
    check("Tab stays inside the dialog and clicks on the covered Spaces tile / dock do nothing", acts() == base and "shell: screen=" not in log().split("shell: modal open")[-1])
    tap("esc")
    check("Escape closes the modal", wait_count("shell: modal closed", 1))
    h3 = until("modal_closed", lambda i: px(i, 600, 650) == CANVAS)
    check("SCREEN: scrim gone; focus returned to About (ring around it)", px(h3, 600, 650) == CANVAS and px(h3, 164, 545) == FOCUS and log().rsplit("shell: focus=", 1)[-1].startswith("16"))

    # ---------------- Escape = Back, uptime ----------------
    click_at(572, 704, "ptr_spaces3")
    check("dock Spaces opens Spaces again (history 3)", wait_last("shell: screen=", "Spaces history=3"))
    tap("esc")
    check("Escape on Spaces goes back (Home, history 2)", wait_last("shell: screen=", "Home history=2"))
    h4 = until("home_late", lambda i: True)
    check("the uptime label changed over time (it is live, not a fixed string)", changed(h0, h4, 760, 20, 1000, 48))
    check("no panic, no failed self-test, still interactive", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())
    print("home_check: %d passed, %d failed" % (passed, failed))
    return 1 if failed else 0

if __name__ == "__main__":
    try: sys.exit(main())
    except Exception as e:
        print("  FAIL: home_check crashed: %r" % (e,)); print("home_check: 0 passed, 1 failed"); sys.exit(1)
