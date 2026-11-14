#!/usr/bin/env python3
"""tests/qemu/gfx_check.py - capture the guest-driven display through QMP `screendump` and verify real pixels.

Standard library only (no Pillow). Used by tests/qemu/gfx-tests.sh; QEMU is started by the shell script, which owns its process.

  gfx_check.py --qmp SOCKET --serial LOG [--timeout SECONDS] [--keep FILE.ppm]

Steps: (1) wait, bounded, for the kernel's serial line "gfx: test pattern drawn"; (2) connect to QMP; (3) repeatedly screendump (PPM)
until the image is 1024x768 and the pattern is present, or the time runs out. ramfb has no present step - QEMU samples guest RAM on its
own refresh timer - so the wait is a bounded poll on the image CONTENT, never a fixed sleep. The time it took is printed.
The 640x480 image QEMU produces with no guest software is the placeholder: it is rejected by the size check alone.

Exit status 0 only if every assertion passed.
"""
import argparse, json, os, socket, sys, time

W, H = 1024, 768
BG, PANEL, BLACK = (32, 48, 64), (224, 224, 224), (0, 0, 0)
RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255)

# (x, y, colour, why). Mirrors tests/kernel-host/gfx_test.c and kernel/gfx/gfx_draw.c (gfx_draw_test_pattern).
EXPECT = [
    (600, 740, BG, "untouched background far from every shape"),
    (64, 700, BG, "background, lower left"), (1000, 400, BG, "background, right edge area"),
    (127, 300, BG, "left of the panel (x=127)"), (896, 300, BG, "right of the panel (x=896)"),
    (300, 95, BG, "above the panel (y=95)"), (300, 672, BG, "below the panel (y=672)"),
    (128, 96, PANEL, "panel top-left pixel"), (895, 671, PANEL, "panel bottom-right pixel"),
    (128, 671, PANEL, "panel bottom-left pixel"), (895, 96, PANEL, "panel top-right pixel"),
    (200, 300, PANEL, "panel interior"), (700, 500, PANEL, "panel interior 2"),
    (300, 382, PANEL, "above the horizontal rule"), (300, 383, BLACK, "horizontal rule, first row"),
    (300, 384, BLACK, "horizontal rule, second row"), (300, 385, PANEL, "below the horizontal rule"),
    (128, 383, BLACK, "horizontal rule, left end"), (895, 384, BLACK, "horizontal rule, right end"),
    (510, 200, PANEL, "left of the vertical rule"), (511, 200, BLACK, "vertical rule, first column"),
    (512, 200, BLACK, "vertical rule, second column"), (513, 200, PANEL, "right of the vertical rule"),
    (511, 96, BLACK, "vertical rule, top end"), (512, 671, BLACK, "vertical rule, bottom end"),
    (192, 160, RED, "red swatch (channel order: R)"), (272, 160, GREEN, "green swatch (channel order: G)"),
    (352, 160, BLUE, "blue swatch (channel order: B)"),
    (159, 160, PANEL, "left of the red swatch"), (160, 128, RED, "red swatch top-left"),
    (223, 191, RED, "red swatch bottom-right"), (224, 160, PANEL, "right of the red swatch"),
    (31, 20, RED, "top-left corner marker"), (1000, 5, GREEN, "top-right corner marker"),
    (20, 745, BLUE, "bottom-left corner marker"), (1000, 745, YELLOW, "bottom-right corner marker"),
    (32, 20, BG, "right of the top-left marker"), (20, 32, BG, "below the top-left marker"),
    (991, 5, BG, "left of the top-right marker"), (20, 735, BG, "above the bottom-left marker"),
    (991, 745, BG, "left of the bottom-right marker"),
    (0, 0, CYAN, "clipped fill from (-20,-20): first visible pixel"), (15, 15, CYAN, "clipped fill: last visible pixel"),
    (16, 16, RED, "just outside the clipped fill"), (20, 5, RED, "marker beside the clipped fill"), (5, 20, RED, "marker below the clipped fill"),
    (1008, 752, MAGENTA, "clipped fill from (1008,752): first pixel"), (1023, 767, MAGENTA, "last pixel of the screen"),
    (1007, 760, YELLOW, "left of the magenta clip"), (1015, 751, YELLOW, "above the magenta clip"),
    (0, 767, BLUE, "bottom-left screen corner"), (1023, 0, GREEN, "top-right screen corner"),
]
PALETTE = {BG, PANEL, BLACK, RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA}


# ---- G3: the shapes image. An independent integer implementation of the documented coverage rule (kernel/gfx/gfx_shape.c):
# a pixel belongs to a rounded rectangle iff its centre is inside and, in a corner quadrant, within the corner circle:
# (2x+1-2cx)^2 + (2y+1-2cy)^2 <= (2r)^2. Written per pixel (no square roots, no spans) so it shares no code or algorithm with the kernel.
S_BG = (16, 24, 40)


def _clamp_r(w, h, r):
    return 0 if (w <= 0 or h <= 0 or r <= 0) else min(r, min(w, h) // 2)


def _inside(x, y, rx, ry, w, h, r):
    if w <= 0 or h <= 0 or x < rx or x >= rx + w or y < ry or y >= ry + h:
        return False
    r = _clamp_r(w, h, r)
    if r == 0:
        return True
    if x < rx + r:
        cx = rx + r
    elif x >= rx + w - r:
        cx = rx + w - r
    else:
        return True
    if y < ry + r:
        cy = ry + r
    elif y >= ry + h - r:
        cy = ry + h - r
    else:
        return True
    dx, dy = 2 * x + 1 - 2 * cx, 2 * y + 1 - 2 * cy
    return dx * dx + dy * dy <= 4 * r * r


def _fill(img, rx, ry, w, h, r, color):
    for y in range(max(ry, 0), min(ry + h, H)):
        for x in range(max(rx, 0), min(rx + w, W)):
            if _inside(x, y, rx, ry, w, h, r):
                img[y * W + x] = color


def _stroke(img, rx, ry, w, h, r, b, color):
    if b <= 0:
        return
    r = _clamp_r(w, h, r)
    ir = max(r - b, 0)
    for y in range(max(ry, 0), min(ry + h, H)):
        for x in range(max(rx, 0), min(rx + w, W)):
            if _inside(x, y, rx, ry, w, h, r) and not _inside(x, y, rx + b, ry + b, w - 2 * b, h - 2 * b, ir):
                img[y * W + x] = color


def shapes_expected():
    """The whole 1024x768 image the G3 kernel must draw, as a list of (r, g, b) tuples."""
    img = [S_BG] * (W * H)
    WH, ORG, CY, GR, DG, WHITE, RD, MG, YL = (240, 240, 240), (255, 128, 0), (0, 160, 255), (0, 200, 80), (60, 60, 60), (255, 255, 255), (255, 0, 0), (255, 0, 255), (255, 255, 0)
    _fill(img, 64, 64, 400, 240, 32, WH)
    _stroke(img, 512, 64, 448, 240, 48, 8, ORG)
    _fill(img, 64, 352, 400, 96, 48, CY)
    _fill(img, 512, 352, 192, 192, 96, GR)
    _fill(img, 736, 352, 224, 192, 24, DG)
    _stroke(img, 736, 352, 224, 192, 24, 4, WHITE)
    _fill(img, -60, -60, 100, 80, 30, RD)
    _fill(img, 944, 688, 160, 120, 40, MG)
    _fill(img, 64, 496, 120, 64, 1000, YL)
    for (x, w, r) in ((240, 1, 5), (256, 2, 1), (272, 3, 1), (288, 5, 2), (312, 8, 4)):
        _fill(img, x, 496, w, w, r, WHITE)
    _stroke(img, 64, 600, 300, 100, 0, 6, (200, 200, 40))
    _stroke(img, 400, 600, 100, 100, 50, 50, (40, 200, 200))
    _stroke(img, 540, 600, 120, 100, 30, 1, WHITE)
    return img


# Hand-derived facts that do not depend on either implementation (worked out from the coverage rule by hand).
SHAPES_ANCHORS = [
    (1000, 20, S_BG, "background, top right"), (20, 740, S_BG, "background, bottom left"),
    (264, 184, (240, 240, 240), "shape A centre"), (64, 64, S_BG, "shape A: the rounded-off corner pixel"),
    (96, 64, (240, 240, 240), "shape A: top edge where the corner arc ends"), (64, 184, (240, 240, 240), "shape A: left edge, straight part"),
    (736, 184, S_BG, "shape B (border only): the middle is background"), (516, 184, (255, 128, 0), "shape B: inside the 8 px rim"),
    (519, 184, (255, 128, 0), "shape B: last rim pixel"), (520, 184, S_BG, "shape B: first pixel past the rim"),
    (608, 448, (0, 200, 80), "shape D (circle) centre"), (512, 352, S_BG, "shape D: bounding-box corner is outside the circle"),
    (736, 352, S_BG, "shape E: rounded corner"), (737, 400, (255, 255, 255), "shape E: white 4 px rim"), (741, 400, (60, 60, 60), "shape E: fill past the rim"),
    (0, 0, (255, 0, 0), "clipped shape at the top-left corner"), (39, 19, S_BG, "clipped shape: its rounded bottom-right corner"),
    (30, 10, (255, 0, 0), "clipped shape: inside"), (40, 5, S_BG, "just right of the clipped shape"),
    (944, 688, S_BG, "clipped bottom-right shape: its rounded top-left corner"), (1023, 767, (255, 0, 255), "clipped bottom-right shape: the last screen pixel is inside it"),
    (1000, 740, (255, 0, 255), "clipped bottom-right shape: inside"),
    (240, 496, (255, 255, 255), "1x1 shape is one pixel"), (241, 496, S_BG, "...and only one"),
    (64, 600, (200, 200, 40), "square-cornered 6 px border: corner pixel"), (69, 650, (200, 200, 40), "...6th rim pixel"), (70, 650, S_BG, "...first inner pixel"),
]


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    pos, toks = 0, []
    while len(toks) < 4:                      # magic, width, height, maxval - with optional '#' comments
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        toks.append(data[start:pos])
    pos += 1                                   # exactly one whitespace byte after maxval
    if toks[0] != b"P6" or toks[3] != b"255":
        raise ValueError("not an 8-bit P6 PPM: %r" % toks)
    w, h = int(toks[1]), int(toks[2])
    pix = data[pos:]
    if len(pix) != w * h * 3:
        raise ValueError("PPM body is %d bytes, expected %d" % (len(pix), w * h * 3))
    return w, h, pix


class Qmp:
    def __init__(self, path, deadline):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(5)
        while True:                            # the socket appears a moment after QEMU starts
            try:
                self.s.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise TimeoutError("QMP socket never appeared: " + path)
                time.sleep(0.1)
        self.buf = b""
        self.read_obj()                        # greeting
        self.cmd("qmp_capabilities")

    def read_obj(self):
        while b"\n" not in self.buf:
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("QMP closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name, **args):
        self.s.sendall(json.dumps({"execute": name, "arguments": args}).encode() + b"\n")
        while True:
            o = self.read_obj()
            if "return" in o:
                return o["return"]
            if "error" in o:
                raise RuntimeError("QMP %s: %s" % (name, o["error"]))


def wait_serial(path, needle, deadline):
    while time.time() < deadline:
        try:
            with open(path, "rb") as f:
                if needle.encode() in f.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.2)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True)
    ap.add_argument("--serial", required=True)
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--keep")
    ap.add_argument("--pattern", choices=("basic", "shapes"), default="basic", help="basic = G2 pattern, shapes = G3 rounded-rectangle image")
    a = ap.parse_args()
    passed = failed = 0

    def check(name, ok, extra=""):
        nonlocal passed, failed
        if ok:
            passed += 1; print("  PASS: " + name)
        else:
            failed += 1; print("  FAIL: " + name + (" (" + extra + ")" if extra else ""))

    t0 = time.time()
    deadline = t0 + a.timeout
    line = "gfx: test pattern drawn" if a.pattern == "basic" else "gfx: shapes pattern drawn"
    got_line = wait_serial(a.serial, line, deadline)
    check("kernel reported the pattern over the serial port (%s)" % line, got_line, "timed out waiting for the line")
    if not got_line:
        print("gfx_check: %d passed, %d failed" % (passed, failed)); return 1
    t_serial = time.time() - t0

    qmp = Qmp(a.qmp, deadline)
    expect_list = EXPECT if a.pattern == "basic" else SHAPES_ANCHORS
    exp_bytes = None
    if a.pattern == "shapes":
        t_ref = time.time()
        exp_bytes = bytes(v for c in shapes_expected() for v in c)
        print("  note: reference image rendered by the independent Python implementation in %.1fs" % (time.time() - t_ref))
    shot = os.path.join(os.path.dirname(os.path.abspath(a.serial)), "screen.ppm")
    attempts, sizes_seen, last_bad = 0, [], []
    img = None
    t1 = time.time()
    while time.time() < deadline:
        attempts += 1
        if os.path.exists(shot):
            os.remove(shot)
        qmp.cmd("screendump", filename=shot, format="ppm")
        w, h, pix = read_ppm(shot)
        if (w, h) not in sizes_seen:
            sizes_seen.append((w, h))
        if (w, h) == (W, H):
            bad = [(x, y, c, why) for (x, y, c, why) in expect_list if tuple(pix[(y * w + x) * 3:(y * w + x) * 3 + 3]) != c]
            if exp_bytes is not None and pix != exp_bytes:
                n = sum(1 for i in range(0, len(pix), 3) if pix[i:i + 3] != exp_bytes[i:i + 3])
                bad.append((0, 0, (0, 0, 0), "%d pixels differ from the reference image" % n))
            last_bad = bad
            if not bad:
                img = (w, h, pix); break
        time.sleep(0.25)
    waited = time.time() - t1
    print("  note: serial line after %.1fs; screendump polled %d time(s) over %.2fs; sizes seen: %s" % (t_serial, attempts, waited, sizes_seen))

    check("screenshot is not the 640x480 QEMU placeholder", (640, 480) not in sizes_seen[-1:] and len(sizes_seen) > 0 and sizes_seen[-1] != (640, 480), "last size %s" % (sizes_seen[-1:],))
    check("screenshot width is 1024", bool(sizes_seen) and sizes_seen[-1][0] == W, "sizes seen %s" % sizes_seen)
    check("screenshot height is 768", bool(sizes_seen) and sizes_seen[-1][1] == H, "sizes seen %s" % sizes_seen)
    if img is None:
        for (x, y, c, why) in last_bad[:12]:
            print("    mismatch at (%d,%d): %s" % (x, y, why))
        check("every expected pixel matched within the time limit", False, "%d mismatches in the last capture" % len(last_bad))
        print("gfx_check: %d passed, %d failed" % (passed, failed)); return 1

    w, h, pix = img
    if a.keep:
        with open(a.keep, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (w, h) + pix)
    for (x, y, c, why) in expect_list:
        check("pixel (%d,%d) %s = %s" % (x, y, why, c), tuple(pix[(y * w + x) * 3:(y * w + x) * 3 + 3]) == c)
    if a.pattern == "shapes":
        check("the screenshot equals the independently rendered reference image, byte for byte (%d pixels)" % (W * H), pix == exp_bytes)
        distinct = len(set(pix[i:i + 3] for i in range(0, len(pix), 3)))
        check("the image has the %d expected distinct colours" % len(set(shapes_expected())), distinct == len(set(shapes_expected())), "got %d" % distinct)
        print("gfx_check: %d passed, %d failed" % (passed, failed))
        return 0 if failed == 0 else 1
    # Whole-image checks: only the nine pattern colours exist (no stale RAM, no stride skew), and each has a sensible size.
    counts = {}
    for i in range(0, len(pix), 3):
        k = (pix[i], pix[i + 1], pix[i + 2]); counts[k] = counts.get(k, 0) + 1
    check("the image contains exactly the nine pattern colours", set(counts) == PALETTE, "extra %s missing %s" % (sorted(set(counts) - PALETTE)[:4], sorted(PALETTE - set(counts))))
    # Exact areas, derived from the drawing order (see gfx_draw_test_pattern): 32x32 corner markers, 64x64 swatches, 16x16 clipped fills.
    black = 768 * 2 + 576 * 2 - 4                       # two rules, 4 pixels where they cross
    panel = 768 * 576 - black - 3 * 64 * 64             # panel minus rules minus swatches
    exp = {RED: 64 * 64 + (32 * 32 - 16 * 16), GREEN: 64 * 64 + 32 * 32, BLUE: 64 * 64 + 32 * 32,
           YELLOW: 32 * 32 - 16 * 16, CYAN: 16 * 16, MAGENTA: 16 * 16, BLACK: black, PANEL: panel}
    exp[BG] = W * H - sum(exp.values())
    for name, c in (("red", RED), ("green", GREEN), ("blue", BLUE), ("yellow", YELLOW), ("cyan", CYAN), ("magenta", MAGENTA),
                    ("black", BLACK), ("panel", PANEL), ("background", BG)):
        check("exact %s pixel count %d" % (name, exp[c]), counts.get(c, 0) == exp[c], "got %d" % counts.get(c, 0))
    total = sum(counts.values())
    check("pixel counts add up to 1024*768", total == W * H, "got %d" % total)
    print("gfx_check: %d passed, %d failed" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:                      # never hide a harness failure
        print("  FAIL: gfx_check crashed: %r" % (e,)); print("gfx_check: 0 passed, 1 failed"); sys.exit(1)
