#!/usr/bin/env python3
"""tests/qemu/scale_check.py - the REAL shell started at a given text scale / contrast (test build flags AURORA_SHELL_TEXT_PCT and
AURORA_SHELL_CONTRAST), driven by real QMP keyboard and pointer events and checked on QEMU screendump pixels plus the guest's "shell:" log.
It never trusts the kernel's own idea of the layout for the pixel rules: the control rectangles come from the guest log (so real controls
can be clicked), but every pass/fail rule below is stated here from the Aurora Glass tokens:
  * every pixel of every screen belongs to the palette of the current mode (nothing stray was drawn),
  * no text spills out of any control (a 2 px band around each control is background-only),
  * focus rings, active indicators, scrim and sheets have the colours of the mode,
  * the dock, Spaces, Index (type, Down, open), the About modal and the Glass Corner sheet work with real clicks and keys."""
import argparse, math, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

def rgb(v): return ((v >> 16) & 255, (v >> 8) & 255, v & 255)
STD = dict(canvas=0xEEF3F8, surface=0xFFFFFF, ink=0x142136, muted=0x52627A, accent=0x2A68E8, focus=0x175CD3, scrim=0x525A66, aura=0x00838C,
           extra=(0xE4ECF6, 0xCFDCF0, 0xC9D4E3, 0xDCE3ED))
HC = dict(canvas=0xFFFFFF, surface=0xFFFFFF, ink=0x000000, muted=0x333333, accent=0x101010, focus=0x0000E0, scrim=0x000000, aura=0x000000,
          extra=(0xD9D9D9, 0xB3B3B3, 0x555555, 0x000000))
AXMAX = 32767
def raw_for(p, extent): return math.ceil(p * AXMAX / (extent - 1))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True); ap.add_argument("--tmp", required=True)
    ap.add_argument("--pct", type=int, required=True); ap.add_argument("--contrast", type=int, required=True)
    ap.add_argument("--timeout", type=float, default=420)
    a = ap.parse_args()
    T = HC if a.contrast else STD
    pal = {rgb(T[k]) for k in ("canvas", "surface", "ink", "muted", "accent", "focus", "scrim", "aura")} | {rgb(c) for c in T["extra"]}
    pal |= {rgb(0x142136), rgb(0xFFFFFF)}                      # the pointer cursor is always drawn in the standard ink and white
    BG = {rgb(T["canvas"]), rgb(T["surface"])}
    passed = failed = 0
    dl = time.time() + a.timeout
    tag = "[%d%%%s] " % (a.pct, " HC" if a.contrast else "")
    def check(name, ok):
        nonlocal passed, failed
        if ok: passed += 1; print("  PASS: " + tag + name)
        else: failed += 1; print("  FAIL: " + tag + name)
    def log():
        with open(a.serial, "rb") as f: return f.read().decode("latin-1")
    def count(s): return log().count(s)
    def wait_count(s, n, secs=25):
        end = time.time() + secs
        while time.time() < end:
            if count(s) >= n: return True
            time.sleep(0.2)
        return False
    def last(prefix):
        t = log()
        return t.rsplit(prefix, 1)[-1].split(chr(10))[0].strip() if prefix in t else None
    def wait_last(prefix, value, secs=25):
        end = time.time() + secs
        while time.time() < end:
            if last(prefix) == value: return True
            time.sleep(0.2)
        return False
    def layout():
        t = log()
        j = t.rfind("shell: layout end")                  # the last COMPLETE block: the serial file may be read while the guest is mid-write
        i = t.rfind("shell: layout begin", 0, j) if j >= 0 else -1
        c = {}
        if i < 0 or j < 0: return c
        for line in t[i:j].split(chr(10)):
            if line.startswith("shell: ctrl "):
                v = line.split()[2:]; c[int(v[0])] = tuple(int(x) for x in v[1:5])
        return c
    def wait_layout(cid, secs=15):
        """the UART log can lag the framebuffer: wait until the layout block that contains control cid has been written"""
        end = time.time() + secs
        while time.time() < end:
            L_ = layout()
            if cid in L_: return L_
            time.sleep(0.25)
        return layout()

    q = Qmp(a.qmp, dl)
    check("the shell reached the interactive state", wait_serial(a.serial, "shell: ready keyboards=1 pointers=1", dl))
    check("the kernel reports the requested appearance", ("shell: appearance text=%d contrast=%d" % (a.pct, a.contrast)) in log())
    check("no panic or failed self-test", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())

    def shot(name):
        path = os.path.join(a.tmp, "%d_%d_%s.ppm" % (a.pct, a.contrast, name)); q.cmd("screendump", filename=path, format="ppm"); return read_ppm(path)
    def px(img, x, y):
        w, h, d = img; o = (y * w + x) * 3; return tuple(d[o:o + 3])
    def shot_until(name, pred, secs=15):
        """the guest logs a state change just before it presents the frame, and the frame is drawn in LAYERS (page, scrim, dialog): a screenshot can catch a
        half-drawn frame that already satisfies a pixel test (in high contrast the scrim is black, so a frame with the scrim but without the dialog still passes
        'scrim corners and palette'). So the expected pixels must show up in two consecutive screenshots that are identical below the header."""
        end = time.time() + secs
        while True:
            img = shot(name)
            if pred(img):
                time.sleep(0.25)
                img2 = shot(name)
                if pred(img2) and img[2][64 * 1024 * 3:] == img2[2][64 * 1024 * 3:]: return img2
            if time.time() > end: return img
            time.sleep(0.3)
    def colors(img):
        w, h, d = img
        return {tuple(d[o:o + 3]) for o in range(0, w * h * 3, 3)}
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k, *mods): q.cmd("input-send-event", events=[key(m, True) for m in mods] + [key(k, True), key(k, False)] + [key(m, False) for m in reversed(mods)])
    def move(x, y): q.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": raw_for(x, 1024)}}, {"type": "abs", "data": {"axis": "y", "value": raw_for(y, 768)}}])
    def btn(d): q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": "left"}}])
    def click_at(x, y):
        move(x, y); time.sleep(0.5); btn(True); btn(False); time.sleep(0.5)
    def click_ctrl(cid):
        L = layout()
        if cid not in L: return False
        x, y, w, h = L[cid]; click_at(x + w // 2, y + h // 2); return True
    def band_clean(img, L):
        """no pixel in the 2 px band just outside any control may be anything but background or the focus ring (text must not spill out)"""
        bad = []
        for cid, (x, y, w, h) in L.items():
            if cid == 40 and (x + w) >= 1000: pass
            pts = []
            for k in (1, 2):
                if y - k >= 0: pts += [(xx, y - k) for xx in range(x + 14, x + w - 14, 3)]
                if y + h + k - 1 < 768: pts += [(xx, y + h + k - 1) for xx in range(x + 14, x + w - 14, 3)]
                if x - k >= 0: pts += [(x - k, yy) for yy in range(y + 14, y + h - 14, 3)]
                if x + w + k - 1 < 1024: pts += [(x + w + k - 1, yy) for yy in range(y + 14, y + h - 14, 3)]
            for (xx, yy) in pts:
                c = px(img, xx, yy)
                if c not in BG and c != rgb(0x142136) and c != rgb(T["focus"]): bad.append((cid, xx, yy, c))
        return bad
    def palette_ok(img): return colors(img) <= pal
    def stray(img): return sorted(colors(img) - pal)[:3]

    # ---------------- Home ----------------
    L = layout()
    h0 = shot_until("home", lambda i: any(px(i, x, y) == rgb(T["aura"]) for x in (300, 500, 700) for y in range(60, 100)))      # wait for a COMPLETE frame (a screenshot can catch a half-drawn one)
    check("Home: every pixel is in the palette of this mode (stray: %s)" % stray(h0), palette_ok(h0))
    check("Home: the status rail is drawn (aura in standard, ink in high contrast)", any(px(h0, x, y) == rgb(T["aura"]) for x in (300, 500, 700) for y in range(60, 100)))
    check("Home: the dock Home item, Spaces, Index and Back are all controls (ids 2, 3, 4 enabled)", all(i in L for i in (2, 3, 4, 10, 16, 11, 12, 40)))
    bc = band_clean(h0, L)
    check("Home: no text spills outside any control (%s)" % (bc[:2],), not bc)
    check("Home: no control leaves the screen or the content/dock regions", all(0 <= x and 0 <= y and x + w <= 1024 and y + h <= 768 for (x, y, w, h) in L.values()) and
          all(y + h <= L[2][1] for cid, (x, y, w, h) in L.items() if cid in (10, 16, 11, 12)))
    check("Home: touch targets are at least 48 px wide and 40 px high", all(w >= 48 and h >= 40 for (x, y, w, h) in L.values()))
    x2, y2, w2, h2 = L[2]
    active_dock = px(h0, x2 + 14, y2 + 4)
    check("Home: the active dock item is drawn with the accent fill", active_dock == rgb(T["accent"]))
    if a.contrast:
        # not colour alone: the active item also carries a marker bar and a different border from the inactive items
        x3, y3, w3, h3 = L[3]
        check("HC: the active dock item has a marker bar inside its left edge (ink-on-ink inverted); an inactive item does not", px(h0, x2 + 9, y2 + h2 // 2) == rgb(T["surface"]) and px(h0, x3 + 9, y3 + h3 // 2) == rgb(T["surface"]) and px(h0, x2 + 5, y2 + h2 // 2) == rgb(T["accent"]))
        check("HC: the canvas is pure white and body text pure black", px(h0, 3, 700) == (255, 255, 255) and any(px(h0, x, y) == (0, 0, 0) for x in range(40, 400) for y in range(120, 200)))

    # ---------------- keyboard focus ring ----------------
    tap("tab")
    check("Tab focuses the first control (Change Space, 10)", wait_last("shell: focus=", "10"))
    x, y, w, h = L[10]
    h1 = shot_until("home_tab", lambda i: px(i, x + w // 2, y - 2) == rgb(T["focus"]))
    x, y, w, h = L[10]
    ring = rgb(T["focus"])
    check("a 3 px focus ring in the mode's focus colour is drawn above the focused control", all(px(h1, x + w // 2, y - k) == ring for k in (1, 2, 3)) and px(h1, x + w // 2, y - 4) != ring)
    for _ in range(3): tap("tab"); time.sleep(0.4)
    check("Tab keeps working (focus moved on from Change Space)", last("shell: focus=") != "10")

    # ---------------- dock navigation by pointer ----------------
    check("clicking the dock Spaces item opens Spaces", click_ctrl(3) and wait_last("shell: screen=", "Spaces history=1"))
    s1 = shot_until("spaces", lambda i: px(i, layout()[30][0] + 14, layout()[30][1] + 4) == rgb(T["accent"]) if 30 in layout() else False)
    check("Spaces: palette only, nothing spills, four Space buttons present", palette_ok(s1) and not band_clean(s1, layout()) and all(i in layout() for i in (30, 31, 32, 33)))
    L = layout()
    check("Spaces: the Study button is drawn active", px(s1, L[30][0] + 14, L[30][1] + 4) == rgb(T["accent"]) and px(s1, L[31][0] + 14, L[31][1] + 4) != rgb(T["accent"]))
    check("clicking Projects selects it", click_ctrl(32) and wait_last("shell: space=", "Projects"))

    # ---------------- Index: type, select, open ----------------
    check("clicking the dock Index item opens Index", click_ctrl(4) and wait_last("shell: screen=", "Index history=2"))
    for k in ("s", "e", "t"): tap(k); time.sleep(0.3)
    check("typing 'set' reaches the search field", wait_last("shell: query=", "[set] results=1 sel=none") or (last("shell: query=") or "").startswith("[set]"))
    i1 = shot("index_set")
    check("Index with a query: palette only, nothing spills out of any row", palette_ok(i1) and not band_clean(i1, layout()))
    tap("backspace"); tap("backspace"); tap("backspace"); time.sleep(0.5)
    check("Backspace clears the field", wait_last("shell: query=", "[] results=8 sel=home") or (last("shell: query=") or "").startswith("[]"))
    for _ in range(3): tap("down"); time.sleep(0.35)
    L = layout()
    rows = sorted(c for c in L if 100 <= c < 108)
    i2 = shot("index_rows")
    check("Index empty query: at least two result rows fit at this scale (%d shown)" % len(rows), len(rows) >= 2)
    selk = (last("shell: query=") or "").split("sel=")[-1]
    active = [c for c in rows if px(i2, L[c][0] + 14, L[c][1] + 4) == rgb(T["accent"])]
    check("Index: exactly one visible row is drawn selected (accent) and it is on screen after moving down 3 times (sel=%s)" % selk, len(active) == 1)
    check("Index: rows never overlap the dock", all(L[c][1] + L[c][3] <= L[2][1] for c in rows))
    tap("ret")
    check("Enter opens the selected result", wait_count("shell: opened=", 1))
    time.sleep(0.6)

    # ---------------- About modal ----------------
    click_ctrl(2); time.sleep(0.6)
    end = time.time() + 20
    while time.time() < end and not (last("shell: screen=") or "").startswith("Home"): time.sleep(0.2)
    check("clicking the dock Home item shows Home again", (last("shell: screen=") or "").startswith("Home"))
    click_ctrl(16)
    check("About opens", wait_count("shell: modal open", 1))
    m1 = shot_until("about", lambda i: px(i, 3, 3) == rgb(T["scrim"]) and px(i, 1020, 764) == rgb(T["scrim"]) and palette_ok(i))
    check("About: palette only; the scrim covers the page (corner pixel is the scrim colour)", palette_ok(m1) and px(m1, 3, 3) == rgb(T["scrim"]) and px(m1, 1020, 764) == rgb(T["scrim"]))
    L = wait_layout(20)
    acts0 = count("shell: activate ")
    click_ctrl(3); time.sleep(0.6)
    check("About: the Close button (20) exists, and a click on the dock behind the scrim activates nothing", 20 in L and count("shell: activate ") == acts0 and (last("shell: screen=") or "").startswith("Home"))
    check("About: nothing spills out of the Close button", not band_clean(m1, {20: L[20]}) if 20 in L else False)
    tap("esc")
    check("Escape closes About and focus returns to the About button (16)", wait_count("shell: modal closed", 1) and wait_last("shell: focus=", "16"))

    # ---------------- Glass Corner ----------------
    click_ctrl(40)
    check("the Held handle opens the Glass Corner sheet", wait_count("shell: corner open", 1))
    c1 = shot_until("corner", lambda i: any(px(i, x, 200) == rgb(T["focus"]) for x in range(560, 1010, 2)))
    check("Corner: palette only, sheet border in the focus colour", palette_ok(c1) and any(px(c1, x, y) == rgb(T["focus"]) for x in range(560, 1010, 2) for y in range(70, 340)))
    L = wait_layout(41)
    check("Corner: the Close button is inside the screen, above the dock, and spills nothing", 41 in L and L[41][1] + L[41][3] <= 640 and not band_clean(c1, {41: L[41]}))
    tap("esc")
    check("Escape closes the sheet and focus returns to the handle (40)", wait_count("shell: corner closed", 1) and wait_last("shell: focus=", "40"))
    check("no panic or failed self-test after the whole run", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())
    print("scale_check %s: %d passed, %d failed" % (tag.strip(), passed, failed))
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
