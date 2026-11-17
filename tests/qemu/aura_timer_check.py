#!/usr/bin/env python3
"""tests/qemu/aura_timer_check.py - Aura timer commands end to end on the REAL shell, broker and SYS_TIMER service. Evidence: real injected key events, the guest's
own lines ("aura: shell ran kind=.. decision=.. state=.. [message]", "shell: clock running secs=N", "shell: clock finished|cancelled", "shell: toast on|off")
and screendump PIXELS (the Clock's countdown really changes). AX states: 1 done, 5 not understood, 6 failed; clarify/rejected parses are state 5."""
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
    def lines(prefix): return [l.rstrip(chr(13)) for l in after_ready().split(chr(10)) if l.startswith(prefix)]
    def ran(): return re.findall(r"aura: shell ran kind=(\w+) decision=(\w+) reason=(\w+) state=(\d+) \[(.*?)\] screen=(\w+) space=(\w+)", after_ready())
    def wait_for(pred, secs=40):
        end = time.time() + secs
        while time.time() < end:
            if pred(): return True
            time.sleep(0.25)
        return False
    def wait_ran(n, secs=30): return wait_for(lambda: len(ran()) >= n, secs)
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
    def tap(k): q.cmd("input-send-event", events=[key(k, True), key(k, False)]); time.sleep(0.15)
    def typ(t):
        for ch in t: tap("spc" if ch == " " else ch)
        time.sleep(0.5)
    def screen_now(): return (re.findall(r"shell: screen=(\w+)", log()) or [None])[-1]
    def shell_focus(): return (re.findall(r"shell: focus=(\w+)", log()) or [None])[-1]
    def tab_to(fid, tries=20):
        for _ in range(tries):
            if shell_focus() == str(fid): return True
            tap("tab"); time.sleep(0.3)
        return shell_focus() == str(fid)
    def wait_screen(name, secs=15): return wait_for(lambda: screen_now() == name, secs)
    def stable():
        a1 = shot(); time.sleep(0.4); a2 = shot()
        return a2 if region(a1, 0, 70, 1024, 660) == region(a2, 0, 70, 1024, 660) else shot()
    def say(cmd):                                       # type a command in Aura, return the new "aura: shell ran" tuple
        n = len(ran()); typ(cmd); tap("ret")
        r = ran()[n] if wait_ran(n + 1) else None
        if r and r[3] != "1": tap("esc"); time.sleep(0.4)      # a command that was not done stays in the field: clear it (a done one is consumed)
        return r
    def running(): return lines("shell: clock running secs=")
    VAL = (56, 223, 700, 260)
    HEAD = (170, 4, 400, 62)
    def is_ink(c): return abs(c[0] - 0x14) < 12 and abs(c[1] - 0x21) < 12 and abs(c[2] - 0x36) < 12
    def ink_px(img, x0, y0, x1, y1): return sum(1 for y in range(y0, y1) for x in range(x0, x1) if is_ink(img[2][(y * img[0] + x) * 3:(y * img[0] + x) * 3 + 3]))

    check("Tab reaches the Aura tile on Home (id 14)", tab_to(14)); tap("ret")
    check("Enter opens the Aura screen", wait_screen("Aura"))
    r = say("start a 1 minute timer")
    check("'start a 1 minute timer': parsed as a timer, allowed by the broker, Done with the canonical preview", r and r[0] == "timer" and r[1] == "allow_now" and r[3] == "1" and r[4] == "Done: Start 1 min timer")
    check("the real timer service accepted it: the shell logged a 60 s running timer", wait_for(lambda: running() == ["shell: clock running secs=60"], 10))
    check("Aura stayed on screen (a timer command does not hijack navigation)", screen_now() == "Aura")
    r = say("start a 5 minute timer")
    check("a second start is refused honestly: Failed (state 6), not Done, and nothing new started", r and r[0] == "timer" and r[3] == "6" and len(running()) == 1)
    r = say("show timer")
    check("'show timer' opens the Clock screen", r and r[0] == "open_screen" and r[3] == "1" and wait_screen("Clock"))
    c1 = stable(); time.sleep(2.3); c2 = stable()
    check("the Clock really counts down: the value row differs 2.3 s apart", region(c1, *VAL) != region(c2, *VAL))
    tap("esc"); check("Escape returns to Aura", wait_screen("Aura"))
    r = say("cancel my timer")
    check("'cancel my timer': Done, and the service cancelled it", r and r[0] == "timer" and r[3] == "1" and wait_for(lambda: lines("shell: clock cancelled"), 10))
    r = say("cancel my timer")
    check("cancelling again: Failed (nothing runs), not Done", r and r[3] == "6")
    r = say("start a 25 minute timer")
    check("'start a 25 minute timer': Done with exactly 1500 seconds", r and r[3] == "1" and r[4] == "Done: Start 25 min timer" and wait_for(lambda: lines("shell: clock running secs=")[-1:] == ["shell: clock running secs=1500"], 10))
    r = say("cancel my timer"); check("cancel the 25 minute timer", r and r[3] == "1")
    n_run = len(running())
    r = say("timer 25")
    check("'timer 25' (no unit) is NOT guessed: a clarification, nothing started", r and r[0] == "clarify" and "How long" in r[4] and len(running()) == n_run)
    r = say("start a timer")
    check("'start a timer' (no duration): clarification, nothing started", r and r[0] == "clarify" and len(running()) == n_run)
    r = say("start a 25 hour timer")
    check("a 25 hour timer is rejected as out of range, nothing started", r and r[0] == "timer" and r[3] != "1" and "1 second to 24 hours" in r[4] and len(running()) == n_run)
    r = say("set an alarm for 7 minutes")
    check("an alarm is not a timer: not done", r and r[3] != "1" and len(running()) == n_run)
    # --- a real 1 minute timer finishing while the Clock is NOT on screen ---
    t_start = time.time()
    r = say("set timer for 1 minute")
    check("'set timer for 1 minute' starts a 60 s timer", r and r[3] == "1" and wait_for(lambda: lines("shell: clock running secs=")[-1:] == ["shell: clock running secs=60"], 10))
    tap("esc"); check("leave Aura for Home", wait_screen("Home") or (tap("esc") or True) and wait_screen("Home"))
    ok = wait_for(lambda: lines("shell: clock finished"), 100)
    waited = time.time() - t_start
    check("the timer finished on the real counter while Home was showing (%.0f s after the command)" % waited, ok)
    check("it was never early (>= 58 s) and prompt (< 80 s)", 58 <= waited < 80)
    check("the completion toast was raised and is really drawn in the header", wait_for(lambda: lines("shell: toast on"), 5) and ink_px(stable(), *HEAD) > 800)
    check("the toast took no focus and navigated nowhere", screen_now() == "Home")
    check("the toast disappears (pixels) and is logged", wait_for(lambda: ink_px(shot(), *HEAD) < 50, 12) and wait_for(lambda: lines("shell: toast off"), 5))
    check("no panic at the end", "KERNEL PANIC" not in log())
    print("aura_timer_check %s: %d passed, %d failed" % (a.scenario, passed, failed))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
