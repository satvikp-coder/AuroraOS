#!/usr/bin/env python3
"""tests/qemu/input_check.py - drive the AURORA_INPUT_TEST kernel through QMP input-send-event and verify BOTH what the guest
reports on the serial log AND what appears in QEMU's own screenshot. A QMP "return" proves nothing: every step waits for the guest's
own line, and every visible claim is checked on pixels read back from `screendump`.

Screen contract (kernel/input/input_demo.c), 1024x768:
  background (16,32,48); key panel (40,40,200,80) grey (64,64,72), blue (32,80,224) while a text key is held, last text char in white;
  modifier boxes y=140..169 at x=40/110/180 (60 wide): lit (240,192,32) while Shift/Ctrl/Alt held, else (48,48,56);
  button (40,200,200,80): red (192,32,32), green (32,192,64) after a complete click; 10x10 white cursor centred on the pointer.
"""
import argparse, math, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

BG, GREY, BLUE, LIT, DIM, RED, GREEN, WHITE = (16, 32, 48), (64, 64, 72), (32, 80, 224), (240, 192, 32), (48, 48, 56), (192, 32, 32), (32, 192, 64), (255, 255, 255)
AXMAX = 32767


def raw_for(px, extent):                       # smallest raw value that the guest scales to pixel px
    return math.ceil(px * AXMAX / (extent - 1))


def scale(raw, extent):                        # the same integer rule as input_scale_axis(), re-derived here
    return raw * (extent - 1) // AXMAX


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True)
    ap.add_argument("--tmp", required=True); ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--mode", choices=("full", "keyboard", "tablet", "none"), default="full")
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

    q = Qmp(a.qmp, dl)
    check("guest reached the input demo", wait_serial(a.serial, "input: ready", dl))
    L = log()
    if a.mode == "none":
        check("no input device: 'devices=0 keyboards=0 pointers=0' and a normal boot", "input: devices=0 keyboards=0 pointers=0 failed=0" in L)
        check("no input device: the demo still runs to completion without a crash", wait_serial(a.serial, "input: demo done", dl) and "KERNEL PANIC" not in log())
        print("input_check: %d passed, %d failed" % (passed, failed)); return 1 if failed else 0
    kb, tb = a.mode in ("full", "keyboard"), a.mode in ("full", "tablet")
    want = "input: devices=%d keyboards=%d pointers=%d failed=0" % (kb + tb, kb, tb)
    check("enumeration: " + want, want in L)
    if tb:
        check("tablet capabilities read from the device: axes 0..32767", "input: axis X range 0..32767" in L and "input: axis Y range 0..32767" in L)
    else:
        check("no tablet: no axis range is reported", "axis X range" not in L)

    def shot(name):
        path = os.path.join(a.tmp, name + ".ppm")
        q.cmd("screendump", filename=path, format="ppm")
        return read_ppm(path)

    def px(img, x, y):
        w, h, d = img; o = (y * w + x) * 3; return tuple(d[o:o + 3])

    def region_has(img, x0, y0, x1, y1, col):
        w, h, d = img
        return any(tuple(d[(y * w + x) * 3:(y * w + x) * 3 + 3]) == col for y in range(y0, y1) for x in range(x0, x1))

    def shot_until(name, pred, secs=15):
        end = time.time() + secs
        while True:
            img = shot(name)
            if pred(img) or time.time() > end: return img
            time.sleep(0.3)

    def key(k, down): return {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": k}}}

    def send(*ev): q.cmd("input-send-event", events=list(ev))

    img0 = shot("initial")
    check("initial screen is 1024x768", img0[0] == 1024 and img0[1] == 768)
    check("initial: background, idle grey panel, red button, unlit modifiers",
          px(img0, 600, 600) == BG and px(img0, 50, 50) == GREY and px(img0, 140, 240) == RED and
          all(px(img0, 70 + i * 70, 155) == DIM for i in range(3)))
    if kb:
        base = count("input: KEY_DOWN code=30")
        send(key("a", True))
        check("guest saw KEY_DOWN for 'a' (evdev code 30) and TEXT 'a'", wait_serial(a.serial, "input: TEXT ch=a", dl) and count("input: KEY_DOWN code=30 mods=0") == base + 1)
        img = shot_until("a_down", lambda i: px(i, 50, 50) == BLUE)
        check("SCREEN: panel turned blue while 'a' is held", px(img, 50, 50) == BLUE and px(img, 230, 110) == BLUE)
        check("SCREEN: the character 'a' is drawn (white pixels inside the panel)", region_has(img, 60, 60, 200, 110, WHITE))
        send(key("a", False))
        check("guest saw KEY_UP for 'a'", wait_serial(a.serial, "input: KEY_UP code=30 mods=0", dl))
        img = shot_until("a_up", lambda i: px(i, 50, 50) == GREY)
        check("SCREEN: panel back to idle grey after release, 'a' remains", px(img, 50, 50) == GREY and region_has(img, 60, 60, 200, 110, WHITE))

        send(key("shift", True), key("a", True))
        check("Shift+a: guest produced TEXT 'A' with mods=1", wait_serial(a.serial, "input: TEXT ch=A", dl) and "input: KEY_DOWN code=30 mods=1" in log())
        img = shot_until("shift_a", lambda i: px(i, 70, 155) == LIT)
        check("SCREEN: Shift box lit, Ctrl and Alt boxes not", px(img, 70, 155) == LIT and px(img, 140, 155) == DIM and px(img, 210, 155) == DIM)
        send(key("a", False), key("shift", False))
        img = shot_until("shift_up", lambda i: px(i, 70, 155) == DIM)
        check("SCREEN: Shift box dark again after release (no stuck modifier)", px(img, 70, 155) == DIM)

        send(key("ctrl", True), key("alt", True))
        img = shot_until("ctrl_alt", lambda i: px(i, 140, 155) == LIT and px(i, 210, 155) == LIT)
        check("SCREEN: Ctrl and Alt boxes lit together", px(img, 140, 155) == LIT and px(img, 210, 155) == LIT and px(img, 70, 155) == DIM)
        before = count("input: TEXT ch=")
        send(key("b", True)); send(key("b", False))
        check("Ctrl+Alt+b: guest saw the key but produced NO text", wait_serial(a.serial, "input: KEY_UP code=48 mods=6", dl) and count("input: TEXT ch=") == before)
        send(key("ctrl", False), key("alt", False))
        shot_until("mods_up", lambda i: px(i, 140, 155) == DIM and px(i, 210, 155) == DIM)

    if tb:
        def move(x, y): send({"type": "abs", "data": {"axis": "x", "value": raw_for(x, 1024)}}, {"type": "abs", "data": {"axis": "y", "value": raw_for(y, 768)}})
        def want_pos(x, y): return "input: PTR_MOVE x=%d y=%d" % (scale(raw_for(x, 1024), 1024), scale(raw_for(y, 768), 768))
        def btn(down): send({"type": "btn", "data": {"down": down, "button": "left"}})
        move(600, 600)
        check("pointer moved to (600,600): guest reports exactly that pixel", wait_serial(a.serial, want_pos(600, 600), dl) and scale(raw_for(600, 1024), 1024) == 600)
        img = shot_until("ptr_600", lambda i: px(i, 600, 600) == WHITE)
        check("SCREEN: cursor square drawn at (600,600), none at the screen centre", px(img, 600, 600) == WHITE and px(img, 596, 604) == WHITE and px(img, 512, 384) == BG)
        n_act = count("input: button activated")
        btn(True); btn(False)
        check("click outside the button: guest saw press+release, both 'inside=0'", wait_serial(a.serial, "input: PTR_UP inside=0", dl) and count("input: PTR_DOWN inside=0") >= 1)
        time.sleep(0.5)
        img = shot("click_outside")
        check("SCREEN: button still red after a click outside it", px(img, 140, 240) == RED and count("input: button activated") == n_act)
        move(140, 240); wait_serial(a.serial, want_pos(140, 240), dl)
        btn(True)
        check("press inside the button recorded (inside=1)", wait_serial(a.serial, "input: PTR_DOWN inside=1", dl))
        move(600, 650); wait_serial(a.serial, want_pos(600, 650), dl)
        btn(False)
        check("release OUTSIDE after pressing inside: not a click", wait_serial(a.serial, "input: PTR_UP inside=0", dl))
        time.sleep(0.5)
        img = shot("drag_off")
        check("SCREEN: button still red after press-inside / release-outside", px(img, 140, 240) == RED and count("input: button activated") == n_act)
        move(140, 240); wait_serial(a.serial, want_pos(140, 240), dl)
        btn(True); btn(False)
        check("press and release both inside: the guest activates the button", wait_serial(a.serial, "input: button activated", dl))
        img = shot_until("clicked", lambda i: px(i, 60, 270) == GREEN)
        check("SCREEN: button turned green (activation visible), cursor drawn on it", px(img, 60, 270) == GREEN and px(img, 140, 240) == WHITE)
        check("SCREEN: exactly one activation recorded", count("input: button activated") == n_act + 1)

    check("no panic in the guest log", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())
    print("input_check: %d passed, %d failed" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print("  FAIL: input_check crashed: %r" % (e,)); print("input_check: 0 passed, 1 failed"); sys.exit(1)
