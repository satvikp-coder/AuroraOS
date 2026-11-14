#!/usr/bin/env python3
"""tests/qemu/ui_check.py - end-to-end check of the native Tier 0 Clear demo (kernel/ui/ui_demo.c, AURORA_UI_DEMO build).
Real events are injected with QMP input-send-event; each step is verified on the guest's own serial log AND on pixels of QEMU's
screendump. The expected colours/positions come from the design tokens and the layout constants of the demo, written out here
independently (not read from the kernel source). Screens: initial, focus ring, activation, modal open (scrim, containment,
no click-through), Escape + focus restoration, pointer click, aborted click, pointer-opened modal.
"""
import argparse, math, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

def rgb(v): return ((v >> 16) & 255, (v >> 8) & 255, v & 255)
CANVAS, SURFACE, INK, ACCENT, FOCUS, SCRIM = map(rgb, (0xEEF3F8, 0xFFFFFF, 0x142136, 0x3478F6, 0x175CD3, 0x525A66))
AXMAX = 32767
def raw_for(px, extent): return math.ceil(px * AXMAX / (extent - 1))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True)
    ap.add_argument("--tmp", required=True); ap.add_argument("--timeout", type=float, default=150)
    ap.add_argument("--keep-png", action="store_true")
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
    check("guest reached the UI demo", wait_serial(a.serial, "ui: ready", dl))
    check("keyboard and tablet enumerated", "ui: devices=2 keyboards=1 pointers=1" in log())

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
    def region_changed(i1, i2, x0, y0, x1, y1):
        return any(px(i1, x, y) != px(i2, x, y) for y in range(y0, y1) for x in range(x0, x1))

    def key(k, down): return {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": k}}}
    def tap(k, *mods):
        q.cmd("input-send-event", events=[key(m, True) for m in mods] + [key(k, True), key(k, False)] + [key(m, False) for m in reversed(mods)])
    def move(x, y): q.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": raw_for(x, 1024)}}, {"type": "abs", "data": {"axis": "y", "value": raw_for(y, 768)}}])
    def btn(d): q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": "left"}}])
    def cursor_at(x, y): return lambda i: px(i, x - 6, y) == INK and px(i, x, y) == SURFACE

    # --- initial screen ---
    i0 = shot("ui0")
    check("initial: canvas, white header and card, dock panel", px(i0, 900, 650) == CANVAS and px(i0, 700, 10) == SURFACE and px(i0, 500, 400) == SURFACE and px(i0, 320, 712) == SURFACE)
    check("initial: Home is the active dock item (accent), Spaces and Index are not", px(i0, 337, 712) == ACCENT and px(i0, 460, 712) == CANVAS and px(i0, 584, 712) == CANVAS)
    check("initial: no focus ring anywhere on the key controls", FOCUS not in (px(i0, 890, 98), px(i0, 388, 688), px(i0, 512, 688), px(i0, 636, 688)))
    check("initial: the pointer cursor is drawn at the screen centre", cursor_at(512, 384)(i0))

    # --- keyboard focus (Tab order = Home, Spaces, Index, Help) ---
    tap("tab")
    check("Tab: focus lands on the first focusable control, Home (id 2)", wait_count("ui: focus 2", 1))
    i1 = until("ui_tab1", lambda i: px(i, 388, 688) == FOCUS)
    check("SCREEN: a 3 px focus ring surrounds Home, outside its edge", all(px(i1, 388, 687 + k) == FOCUS for k in range(3)) and px(i1, 388, 686) != FOCUS and px(i1, 329, 712) == FOCUS)
    check("SCREEN: no ring on the other controls", FOCUS not in (px(i1, 890, 98), px(i1, 512, 688), px(i1, 636, 688)))
    tap("tab")
    check("Tab: focus moves to Spaces (id 3)", wait_count("ui: focus 3", 1))
    i2 = until("ui_tab2", lambda i: px(i, 512, 688) == FOCUS)
    check("SCREEN: ring moved to Spaces; Home's ring is gone", px(i2, 512, 688) == FOCUS and px(i2, 388, 688) != FOCUS)
    tap("ret")
    check("Enter on Spaces: the guest activates id 3", wait_count("ui: activate 3", 1))
    i3 = until("ui_enter", lambda i: px(i, 460, 712) == ACCENT)
    check("SCREEN: Spaces became the active item, Home is no longer active", px(i3, 460, 712) == ACCENT and px(i3, 337, 712) == CANVAS and px(i3, 584, 712) == CANVAS)
    check("SCREEN: the status label text changed (Active: Home -> Active: Spaces)", region_changed(i0, i3, 40, 100, 440, 148))
    check("SCREEN: focus ring stays on the activated Spaces button", px(i3, 512, 688) == FOCUS)
    tap("tab", "shift")
    check("Shift+Tab: focus moves back to Home (id 2)", wait_count("ui: focus 2", 2))
    tap("tab", "shift")
    check("Shift+Tab from the first control wraps to the last, Help (id 1)", wait_count("ui: focus 1", 1))
    until("ui_wrap", lambda i: px(i, 890, 98) == FOCUS)

    # --- modal via keyboard ---
    tap("spc")
    check("Space on Help opens the modal", wait_count("ui: modal open", 1) and wait_count("ui: activate 1", 1))
    i4 = until("ui_modal", lambda i: px(i, 600, 80) == SCRIM)
    check("SCREEN: scrim covers the content and the dock; dialog is white on top of it", px(i4, 600, 80) == SCRIM and px(i4, 340, 712) == SCRIM and px(i4, 330, 350) == SURFACE)
    check("check: Help (focus was there) is hidden by the scrim; Close has the focus ring", wait_count("ui: focus 5", 1) and px(i4, 512, 398) == FOCUS)
    for _ in range(3): tap("tab")
    time.sleep(0.8)
    i5 = shot("ui_modal_tab")
    check("Tab x3 inside the dialog: focus never leaves it (ring still on Close, no ring behind the scrim)", px(i5, 512, 398) == FOCUS and px(i5, 890, 98) != FOCUS and count("ui: focus 2") == 2 and count("ui: focus 3") == 1)
    acts = count("ui: activate ")
    move(340, 712); until("ui_ptr_cov", cursor_at(340, 712))
    btn(True); btn(False); time.sleep(1.0)
    check("click on the dock button hidden under the modal does nothing (no click-through)", count("ui: activate ") == acts)
    tap("esc")
    check("Escape closes the modal", wait_count("ui: modal closed", 1))
    i6 = until("ui_closed", lambda i: px(i, 600, 80) == CANVAS and px(i, 890, 98) == FOCUS)
    check("SCREEN: scrim gone, content visible again", px(i6, 600, 80) == CANVAS and px(i6, 500, 400) == SURFACE)
    check("SCREEN: focus returned to Help (ring on Help again)", px(i6, 890, 98) == FOCUS and px(i6, 512, 398) != FOCUS)

    # --- pointer ---
    move(636, 712); until("ui_ptr_idx", cursor_at(636, 712))
    btn(True); btn(False)
    check("pointer click on Index activates id 4", wait_count("ui: activate 4", 1))
    i7 = until("ui_idx", lambda i: px(i, 584, 712) == ACCENT)
    check("SCREEN: Index is now the active item; Spaces and Home are not", px(i7, 584, 712) == ACCENT and px(i7, 460, 712) == CANVAS and px(i7, 337, 712) == CANVAS)
    acts = count("ui: activate ")
    move(400, 712); until("ui_ptr_home", cursor_at(400, 712))
    btn(True); move(900, 400); until("ui_ptr_away", cursor_at(900, 400)); btn(False); time.sleep(1.0)
    check("press on Home, drag away, release elsewhere: not a click", count("ui: activate ") == acts)
    move(320, 740); until("ui_ptr_gap", cursor_at(320, 740))
    btn(True); btn(False); time.sleep(0.8)
    check("click on the dock background (no button) does nothing", count("ui: activate ") == acts)
    move(892, 124); until("ui_ptr_help", cursor_at(892, 124))
    btn(True); btn(False)
    check("pointer click on Help opens the modal", wait_count("ui: modal open", 2))
    move(512, 424); until("ui_ptr_close", cursor_at(512, 424))
    btn(True); btn(False)
    check("pointer click on Close closes the modal", wait_count("ui: modal closed", 2))
    i8 = until("ui_final", lambda i: px(i, 600, 80) == CANVAS)
    check("SCREEN: final frame has no scrim and Index is still active", px(i8, 600, 80) == CANVAS and px(i8, 584, 712) == ACCENT)
    check("no panic in the guest log", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())
    print("ui_check: %d passed, %d failed" % (passed, failed))
    return 1 if failed else 0

if __name__ == "__main__":
    try: sys.exit(main())
    except Exception as e:
        print("  FAIL: ui_check crashed: %r" % (e,)); print("ui_check: 0 passed, 1 failed"); sys.exit(1)
