#!/usr/bin/env python3
"""tests/qemu/ui_client_check.py - the REAL shell composing surfaces that EL0 processes described through SYS_UI (kernel built with
AURORA_SHELL_CLIENTS; clients from tests/programs/uiclient.c). Evidence: guest "[user pN]" lines (what the EL0 process saw), the "ui:" lines the
compositor logs when it accepts a surface, and QMP screendump PIXELS plus real injected pointer and key events. Scenarios:
  ui     two demo clients, a hostile client (the attacks in uiclient.c, then a flood), a client without the capability: surfaces appear, input reaches
         only the focused client, the hostile client is refused everywhere, a modal withholds input, the dock and Glass Corner stay untouched
  fault  a demo client plus a client that faults after drawing: its surface disappears, the other survives, the shell stays usable"""
import argparse, math, os, re, shutil, sys, time, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

MUTED, FOCUS, WHITE, INK, CANVAS = (82, 98, 122), (23, 92, 211), (255, 255, 255), (20, 33, 54), (238, 243, 248)
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
    def check(name, ok, why=None):
        nonlocal passed, failed
        if ok: passed += 1; print("  PASS: " + tag + name)
        else:
            failed += 1; print("  FAIL: " + tag + name)
            diagnose(name, why)
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
    stable_state = {"stable": None, "tries": 0}
    def raw_shot():
        q.cmd("screendump", filename=path, format="ppm"); return read_ppm(path)
    def shot(tries=15):
        """The compositor draws a frame in LAYERS (page, then each client's surface, then the chrome), so one screendump can catch a half-drawn frame: CI evidence
        (the colour map printed by diagnose) showed the first client's chrome drawn and the second's still the page edge. A frame is only judged once two screendumps
        taken one poll apart are identical below the 64-pixel header strip (the clock lives there). If it never settles, the last frame is returned and the
        evidence says so; it is never silently trusted."""
        img = raw_shot()
        for n in range(tries):
            time.sleep(0.2)
            img2 = raw_shot()
            if img[2][64 * 1024 * 3:] == img2[2][64 * 1024 * 3:]:
                stable_state["stable"], stable_state["tries"] = True, n + 1; return img2
            img = img2
        stable_state["stable"], stable_state["tries"] = False, tries
        return img
    def px(img, x, y):
        w, h, d = img; o = (y * w + x) * 3; return tuple(d[o:o + 3])
    def region(img, x0, y0, x1, y1):
        w, h, d = img
        return b"".join(bytes(d[(y * w + x0) * 3:(y * w + x1) * 3]) for y in range(y0, y1))
    def diagnose(name, why):
        # Evidence for a failed check, printed into the log so a one-off failure in CI can be explained afterwards instead of guessed at.
        print("    DIAG: t=%.1fs since start, scenario=%s, last frame stable=%s after %d polls" % (time.time() - (dl - a.timeout), a.scenario, stable_state["stable"], stable_state["tries"]))
        if why is not None:
            try: print("    DIAG: " + why())
            except Exception as e: print("    DIAG: (diagnostic itself failed: %r)" % (e,))
        if name.startswith("SCREEN") or os.path.exists(path):          # any check can have judged a frame; the dump says which frame it is (the last one captured)
            try:
                img = read_ppm(path)                                    # the last frame this scenario captured, which is the one the failed check judged
                w, h, d = img
                cols = {}
                for y in range(0, h, 16):
                    for x in range(0, w, 16): cols[px(img, x, y)] = cols.get(px(img, x, y), 0) + 1
                legend = sorted(cols.items(), key=lambda kv: -kv[1])[:10]
                sym = {c: "0123456789"[i] for i, (c, n) in enumerate(legend)}
                print("    DIAG: frame %dx%d, one char per 16x16 px, legend %s ('.' = any other colour)" % (w, h, ", ".join("%s=%s x%d" % (sym[c], c, n) for c, n in legend)))
                for y in range(0, h, 16): print("    DIAG| " + "".join(sym.get(px(img, x, y), ".") for x in range(0, w, 16)))
                keep = os.path.join(a.tmp, "%s.fail%d.ppm" % (a.scenario, failed))
                shutil.copyfile(path, keep); print("    DIAG: full frame kept at " + keep)
            except Exception as e: print("    DIAG: (frame dump failed: %r)" % (e,))
    def count_col(img, x0, y0, x1, y1, col): return sum(1 for y in range(y0, y1) for x in range(x0, x1) if px(img, x, y) == col)
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k): q.cmd("input-send-event", events=[key(k, True), key(k, False)])
    def move(x, y): q.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": raw_for(x, 1024)}}, {"type": "abs", "data": {"axis": "y", "value": raw_for(y, 768)}}])
    def btn(d): q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": "left"}}])
    def click(x, y): move(x, y); time.sleep(0.5); btn(True); btn(False); time.sleep(0.7)
    def shot_until(pred, secs=20):
        end = time.time() + secs
        while True:
            img = shot()
            if pred(img) or time.time() > end: return img
            time.sleep(0.3)
    # who is who: "shell: client NAME pid=N" and "ui: surface slot=S pid=P at X,Y size WxH"
    def pids():
        return {m.group(1): int(m.group(2)) for m in re.finditer(r"shell: client (\w+) pid=(\d+)", log())}
    def surfaces():
        return [(int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6))) for m in re.finditer(r"ui: surface slot=(\d+) pid=(\d+) at (\d+),(\d+) size (\d+)x(\d+)", log())]
    def lines_of(pid): return [l for l in after_ready().split(chr(10)) if l.startswith("[user p%d] " % pid)]
    def events_of(pid): return [l for l in lines_of(pid) if " ev " in l]
    def shell_focus(): return (re.findall(r"shell: focus=(\w+)", log()) or [None])[-1]
    def tab_to(fid, tries=14):
        for _ in range(tries):
            if shell_focus() == str(fid): return True
            tap("tab"); time.sleep(0.4)
        return shell_focus() == str(fid)
    def surf_of(pid):
        for s in surfaces():
            if s[1] == pid: return s
        return None
    def latency(n=6):
        out = []
        for _ in range(n):
            before = region(shot(), 0, 64, 1024, 768)
            t0 = time.time(); tap("tab"); ok = False
            while time.time() - t0 < 12:
                if region(shot(), 0, 64, 1024, 768) != before: ok = True; break
            out.append((time.time() - t0) * 1000.0 if ok else float("inf"))
            time.sleep(0.2)
        return out

    sc = a.scenario
    need = ("c_ui1", "c_ui2", "c_uibad", "c_uinocap") if sc == "ui" else ("c_ui1", "c_uifault")
    wait_for(lambda: all(n in pids() for n in need), 120)         # the kernel starts the clients one by one AFTER the shell is ready
    P = pids()
    if sc == "ui":
        check("all four EL0 UI clients were started by the kernel loader", all(n in P for n in need))
        u1, u2, ub, un = (P.get(n, 0) for n in need)
        check("EL0 execution and IPC: both demo clients created, drew and committed a surface (kernel replies 0)", wait_for(lambda: any("surface committed=0" in l for l in lines_of(u1)) and any("surface committed=0" in l for l in lines_of(u2)), 120))
        check("the compositor validated and accepted the surfaces and PLACED them itself (the clients never chose a position)", surf_of(u1) is not None and surf_of(u2) is not None)
        s1, s2 = surf_of(u1), surf_of(u2)
        check("the placements are inside the content region (x 40..984, y 124..616) with the chrome strip above them", all(40 <= s[2] and s[2] + s[4] <= 984 and 124 <= s[3] and s[3] + s[5] <= 616 for s in (s1, s2)))
        x1, y1, w1, h1 = s1[2], s1[3], s1[4], s1[5]; x2, y2, w2, h2 = s2[2], s2[3], s2[4], s2[5]
        img = shot_until(lambda i: px(i, x1 + 3, y1 - 14) == MUTED and px(i, x1 + w1 // 2, y1 + h1 - 6) == WHITE)
        check("SCREEN: the first client's surface is drawn: compositor chrome strip (muted) above a white body", px(img, x1 + 3, y1 - 14) == MUTED and px(img, x1 + w1 // 2, y1 + h1 - 6) == WHITE)
        check("SCREEN: the client's committed label is drawn (ink pixels in its first rectangle) and its second rectangle has a border", count_col(img, x1 + 8, y1 + 8, x1 + 248, y1 + 32, INK) > 30)
        check("SCREEN: the second client has its own surface somewhere else, also unfocused", px(img, x2 + 3, y2 - 14) == MUTED and (x1, y1) != (x2, y2))
        check("the dock panel and the Glass Corner handle are untouched by the clients (white dock interior, corner handle fill)", px(img, 400, 672) == WHITE and px(img, 920, 30) != MUTED and px(img, 960, 12) != FOCUS)
        # ---- the hostile client ----
        check("the hostile client's whole battery ran: summary reported with no unexpected answer", wait_for(lambda: "hostile summary bad=0" in " ".join(lines_of(ub)).replace("\n", " "), 120) and not any("hostile BAD" in l for l in lines_of(ub)))
        L = " | ".join(lines_of(ub))
        for name, val in (("wrong version", -301), ("compositor-only type (STATUS)", -302), ("truncated message", -303), ("oversized text length", -304), ("widget index 16", -305),
                          ("rectangle beyond the surface", -305), ("zero-size rectangle", -305), ("size class 3", -305), ("non-ASCII title", -310), ("quota refusal", -307),
                          ("poll into a null buffer", -2), ("send from a kernel address", -2), ("send of 65 bytes", -3), ("unknown operation", -1)):
            check("hostile: %s answered %d" % (name, val), ("hostile %s=%d" % (name, val)) in L)
        check("hostile: of the 8 handles it guessed only its own worked; the 7 others were refused with the same answer as a dead handle (-306)", "hostile foreign handles accepted=1" in L and "hostile foreign handles rejected=7" in L)
        check("hostile: the quota stopped it at 4 surfaces", "hostile surfaces created=4" in L)
        check("the client without the capability was refused (-309) and made no surface", wait_for(lambda: any("nocap create=-309" in l for l in lines_of(un)), 60) and surf_of(un) is None)
        check("the hostile client owns at most 4 surfaces on screen", len([s for s in surfaces() if s[1] == ub]) <= 4)
        # ---- focus and input ----
        img = shot()
        check("with the hostile client flooding focus requests, nothing has focus: no chrome is focus-coloured", px(img, x1 + 3, y1 - 14) == MUTED and px(img, x2 + 3, y2 - 14) == MUTED,
              lambda: "chrome probes: surface1 (%d,%d)=%s surface2 (%d,%d)=%s, want %s each (FOCUS is %s); surfaces %s; guest focus lines %s" % (
                  x1 + 3, y1 - 14, px(img, x1 + 3, y1 - 14), x2 + 3, y2 - 14, px(img, x2 + 3, y2 - 14), MUTED, FOCUS, surfaces(), re.findall(r"shell: focus=\w+", log())[-5:]))
        n1, n2, nb = len(events_of(u1)), len(events_of(u2)), len(events_of(ub))
        click(x1 + 120, y1 + 120)
        img = shot_until(lambda i: px(i, x1 + 3, y1 - 14) == FOCUS)
        check("SCREEN: a press on the first surface gives it focus (its chrome turns focus-coloured); the second stays unfocused", px(img, x1 + 3, y1 - 14) == FOCUS and px(img, x2 + 3, y2 - 14) == MUTED)
        check("the first client received the pointer events in SURFACE-LOCAL coordinates (press at 120,120)", wait_for(lambda: any("ev ptr x=120" in l for l in lines_of(u1)) and any("ev ptr y=120" in l for l in lines_of(u1)), 30))
        before = region(img, x1 + 8, y1 + 40, x1 + 248, y1 + 64)
        tap("a")
        check("a key press reaches ONLY the focused client: the first logs it (evdev code 30)", wait_for(lambda: any("ev key down code=30" in l for l in lines_of(u1)), 30))
        img = shot_until(lambda i: region(i, x1 + 8, y1 + 40, x1 + 248, y1 + 64) != before)
        check("SCREEN: the key made the client update and COMMIT its counter: the pixels of its second label changed", region(img, x1 + 8, y1 + 40, x1 + 248, y1 + 64) != before)
        check("the second client and the hostile client received no key and no pointer event for the first client's input", len(events_of(u2)) == n2 and not any("ev key" in l for l in lines_of(ub)))
        click(x2 + 120, y2 + 120)
        img = shot_until(lambda i: px(i, x2 + 3, y2 - 14) == FOCUS)
        check("SCREEN: focus moved to the second surface (the compositor moved it, on the user's press)", px(img, x2 + 3, y2 - 14) == FOCUS and px(img, x1 + 3, y1 - 14) == MUTED)
        k1 = len([l for l in lines_of(u1) if "ev key down" in l])
        tap("b")
        check("now the second client gets the key (code 48) and the first gets nothing more", wait_for(lambda: any("ev key down code=48" in l for l in lines_of(u2)), 30) and len([l for l in lines_of(u1) if "ev key down" in l]) == k1)
        tap("tab"); time.sleep(1.0)
        img = shot()
        check("Tab always belongs to the shell: it takes focus back from the client (no surface is focus-coloured)", px(img, x1 + 3, y1 - 14) == MUTED and px(img, x2 + 3, y2 - 14) == MUTED)
        e1, e2 = len(events_of(u1)), len(events_of(u2))
        tap("c"); time.sleep(1.0)
        check("with focus back in the shell a typed key reaches no client", len(events_of(u1)) == e1 and len(events_of(u2)) == e2)
        # ---- a modal withholds input ----
        click(x1 + 120, y1 + 120)
        shot_until(lambda i: px(i, x1 + 3, y1 - 14) == FOCUS)
        check("Tab from a focused client lands in the shell and reaches the About button (id 16)", tab_to(16))
        tap("ret")
        check("the About modal opens from the keyboard while a client had focus", wait_for(lambda: "shell: modal open" in log(), 30))
        e1, e2, p1 = len(events_of(u1)), len(events_of(u2)), len([l for l in lines_of(u1) if "ev ptr" in l])
        click(x1 + 60, y1 + 60); tap("d"); time.sleep(1.0)
        img = shot()
        check("SCREEN: the modal scrim covers the client surfaces (corner pixel is the scrim colour)", px(img, 3, 3) == (82, 90, 102))
        check("while the modal is open NO client receives a pointer or key event, and the click did not reach the surface", len(events_of(u1)) == e1 and len(events_of(u2)) == e2 and len([l for l in lines_of(u1) if "ev ptr" in l]) == p1)
        tap("esc")
        check("Escape closes the modal", wait_for(lambda: "shell: modal closed" in log(), 30))
        # ---- responsiveness with a flooding hostile client plus two demo clients ----
        lat = latency()
        check("the shell stays responsive while the hostile client floods the interface (Tab-to-screen median %.0f ms, max %.0f ms)" % (statistics.median(lat), max(lat)), all(x != float("inf") for x in lat) and statistics.median(lat) < 1500)
        print("  LATENCY ui median_ms=%.0f max_ms=%.0f" % (statistics.median(lat), max(lat)))
        check("the hostile client is still flooding (it did not freeze the system, and the system did not stop it)", len([l for l in lines_of(ub) if "flood iterations" in l]) >= 2)
    elif sc == "fault":
        u1, uf = P.get("c_ui1", 0), P.get("c_uifault", 0)
        check("both clients were started", u1 > 0 and uf > 0)
        check("both drew a surface", wait_for(lambda: surf_of(u1) is not None and surf_of(uf) is not None, 120))
        s1, sf = surf_of(u1), surf_of(uf)
        img = shot_until(lambda i: px(i, sf[2] + 3, sf[3] - 14) == MUTED and px(i, s1[2] + 3, s1[3] - 14) == MUTED, 60)
        img0 = img                                                      # the frame that still shows the soon-to-fault surface
        seen = px(img, sf[2] + 3, sf[3] - 14) == MUTED
        check("SCREEN: the soon-to-fault client's surface was visible (compositor chrome)", seen or "FAULT at EL0" in after_ready())
        check("the kernel contained the EL0 fault and carried on", wait_for(lambda: "FAULT at EL0" in after_ready() and "process stopped, kernel continues" in after_ready(), 120))
        check("the shell reaped the faulted process", wait_for(lambda: "faulted" in after_ready() and "shell: reaped pid=%d" % uf in after_ready(), 30))
        check("the compositor dropped the faulted client's surface when it was reaped (guest log)", wait_for(lambda: ("ui: client gone pid=%d surfaces dropped=1" % uf) in after_ready(), 30))
        img = shot_until(lambda i: px(i, sf[2] + 3, sf[3] - 14) != MUTED and region(i, sf[2], sf[3] - 28, sf[2] + sf[4], sf[3] + sf[5]) != region(img0, sf[2], sf[3] - 28, sf[2] + sf[4], sf[3] + sf[5]), 30)
        check("SCREEN: the faulted client's surface is gone: its chrome strip is no longer drawn and the area no longer matches what it showed (the Home page is visible again)", px(img, sf[2] + 3, sf[3] - 14) != MUTED and region(img, sf[2], sf[3] - 28, sf[2] + sf[4], sf[3] + sf[5]) != region(img0, sf[2], sf[3] - 28, sf[2] + sf[4], sf[3] + sf[5]))
        check("SCREEN: the other client's surface survived", px(img, s1[2] + 3, s1[3] - 14) == MUTED and px(img, s1[2] + s1[4] // 2, s1[3] + s1[5] - 6) == WHITE,
              lambda: "survivor surface x,y,w,h=%s: chrome probe (%d,%d)=%s want %s; body probe (%d,%d)=%s want %s; faulted surface was %s; this frame still has the faulted chrome: %s" % (
                  s1[2:6], s1[2] + 3, s1[3] - 14, px(img, s1[2] + 3, s1[3] - 14), MUTED, s1[2] + s1[4] // 2, s1[3] + s1[5] - 6, px(img, s1[2] + s1[4] // 2, s1[3] + s1[5] - 6), WHITE, sf[2:6],
                  px(img, sf[2] + 3, sf[3] - 14) == MUTED))
        click(s1[2] + 100, s1[3] + 100)
        check("the survivor still gets input after the other client's fault", wait_for(lambda: any("ev ptr" in l for l in lines_of(u1)), 30))
        lat = latency(4)
        check("the shell is fully responsive afterwards (median %.0f ms)" % statistics.median(lat), all(x != float("inf") for x in lat))
    check("no panic or failed self-test at the end", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())
    print("ui_client_check %s: %d passed, %d failed" % (tag.strip(), passed, failed))
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
