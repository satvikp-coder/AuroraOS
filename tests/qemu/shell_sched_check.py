#!/usr/bin/env python3
"""tests/qemu/shell_sched_check.py - the REAL native shell with EL0 clients running at the same time (kernel built with AURORA_SHELL_CLIENTS).
Evidence that does not rest on the UART alone: input is injected through QMP and the SCREEN is polled until it changes (that is the
latency measured), while "[user pN]" lines from the guest prove the EL0 clients really executed. Scenarios (see shell-sched-tests.sh):
  none      no clients: baseline latency, no scheduler activity, the shell idles
  spin      two CPU-bound clients: both make progress (fair), input stays responsive
  yblock    a yielding client and a client blocked on IPC: the blocked one costs nothing
  exitfault one client exits normally, one faults: both are reaped, the shell keeps working
  churn     25 short-lived clients are created and destroyed in sequence while the shell is used
  noinput   display but no input devices: clients still run"""
import argparse, os, sys, time, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

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
    def wait_for(pred, secs=30):
        end = time.time() + secs
        while time.time() < end:
            if pred(): return True
            time.sleep(0.25)
        return False
    q = Qmp(a.qmp, dl)
    check("the real kernel reached the interactive shell", wait_serial(a.serial, "shell: ready keyboards=", dl))
    has_input = a.scenario != "noinput"
    check("no panic or failed self-test", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())
    path = os.path.join(a.tmp, a.scenario + ".ppm")
    def shot():
        q.cmd("screendump", filename=path, format="ppm"); return read_ppm(path)
    def region(img, x0, y0, x1, y1):
        w, h, d = img
        return b"".join(bytes(d[(y * w + x0) * 3:(y * w + x1) * 3]) for y in range(y0, y1))
    def key(k, d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    def tap(k): q.cmd("input-send-event", events=[key(k, True), key(k, False)])
    def latency(n=8):
        """press Tab, poll the screen until it changes below the header (the header holds the uptime clock; the dock at y 664-744 must be included: focus moves there too); returns the latencies in ms (inf if the screen never changed)"""
        out = []
        for _ in range(n):
            before = region(shot(), 0, 64, 1024, 768)
            t0 = time.time(); tap("tab")
            ok = False
            while time.time() - t0 < 12:
                if region(shot(), 0, 64, 1024, 768) != before: ok = True; break
            out.append((time.time() - t0) * 1000.0 if ok else float("inf"))
            if not ok:                     # keep the evidence: what the guest logged about this press, and the last frames
                fl = [l for l in log().split(chr(10)) if l.startswith("shell: focus=")]
                print("  NOTE: Tab press %d of %d changed nothing on screen within 12 s; guest focus log tail %s; last slice line: %s" % (len(out), n, fl[-4:], (sched_lines() or ["none"])[-1]))
            time.sleep(0.2)
        return out
    def after_ready():
        t = log(); i = t.find("shell: ready keyboards=")
        return t[i:] if i >= 0 else ""        # the boot self-tests also print "[user pN]" lines: only what happens after the shell is interactive counts
    def users(pid=None):
        return [l for l in after_ready().split(chr(10)) if l.startswith("[user p") and (pid is None or l.startswith("[user p%d]" % pid))]
    def sched_lines(): return [l for l in after_ready().split(chr(10)) if l.startswith("shell: sched slices=")]
    def all_changed(lat): return all(x != float("inf") for x in lat)

    sc = a.scenario
    if sc == "none":
        time.sleep(3)
        check("with no EL0 client the scheduler is never entered (no slice summary, no user line)", not sched_lines() and not users())
        lat = latency()
        check("Tab changes the screen every time (baseline latency median %.0f ms, max %.0f ms)" % (statistics.median(lat), max(lat)), all_changed(lat))
        print("  LATENCY none median_ms=%.0f max_ms=%.0f" % (statistics.median(lat), max(lat)))
    elif sc in ("spin", "noinput"):
        check("both CPU-bound clients were started (the kernel launches them one by one AFTER the shell is ready, so wait for both)", wait_for(lambda: "shell: client c_spin1 pid=" in log() and "shell: client c_spin2 pid=" in log(), 120))
        check("EL0 execution is visible after the shell is interactive: both spinners print heartbeats", wait_for(lambda: len(users(1)) >= 2 and len(users(2)) >= 2, 90))
        n1, n2 = len(users(1)), len(users(2))
        time.sleep(8)
        d1, d2 = len(users(1)) - n1, len(users(2)) - n2
        check("fair scheduling: during 8 more seconds BOTH clients kept running (%d and %d new heartbeats)" % (d1, d2), d1 >= 2 and d2 >= 2 and d1 <= 4 * d2 and d2 <= 4 * d1)
        sl = sched_lines()
        check("scheduler slices are handed out and the two clients preempt each other", len(sl) >= 3 and "preemptions=0 " not in sl[-1])
        if has_input:
            lat = latency()
            check("Tab changes the screen every time while two clients spin (median %.0f ms, max %.0f ms)" % (statistics.median(lat), max(lat)), all_changed(lat))
            check("input latency stays bounded with two CPU-bound clients (median under 1500 ms, max under 6000 ms)", statistics.median(lat) < 1500 and max(lat) < 6000)
            print("  LATENCY spin median_ms=%.0f max_ms=%.0f" % (statistics.median(lat), max(lat)))
            check("the heartbeats continued during the input test", len(users(1)) > n1 + d1 and len(users(2)) > n2 + d2)
        else:
            check("with no input device the shell reports it and the clients run regardless", "shell: ready keyboards=0 pointers=0" in log())
    elif sc == "yblock":
        check("the yielder runs (yield lines appear)", wait_for(lambda: any("yield count=" in l for l in users()), 90))
        check("the blocked client printed its line once and then ran no more", len([l for l in users() if "block waiting" in l]) == 1)
        y0 = len([l for l in users() if "yield count=" in l])
        time.sleep(6)
        y1 = len([l for l in users() if "yield count=" in l])
        check("the yielder kept running (%d new lines)" % (y1 - y0), y1 > y0)
        lat = latency()
        check("Tab changes the screen every time (median %.0f ms, max %.0f ms)" % (statistics.median(lat), max(lat)), all_changed(lat))
        print("  LATENCY yblock median_ms=%.0f max_ms=%.0f" % (statistics.median(lat), max(lat)))
    elif sc == "exitfault":
        check("the exiting client exited with code 7 and was reaped", wait_for(lambda: "exited code=7" in after_ready(), 120))
        check("the faulting client was contained: the kernel logged the EL0 fault and carried on", wait_for(lambda: "FAULT at EL0" in after_ready(), 120) and "process stopped, kernel continues" in after_ready())
        check("the faulted process was reaped too", wait_for(lambda: " faulted" in log() and "shell: reaped pid=" in log(), 30))
        time.sleep(2)
        lat = latency()
        check("the shell is still fully responsive after an exit and a fault (median %.0f ms)" % statistics.median(lat), all_changed(lat))
        n = len(sched_lines())
        time.sleep(4)
        check("nothing is left to run: no further slice summaries once the clients are gone", len(sched_lines()) == n)
    elif sc == "churn":
        check("25 short-lived clients were created, ran, exited and were reaped while the shell stayed up", wait_for(lambda: "shell: churn done started=25 failed=0" in log(), 300))
        check("every one of them was reaped (25 reap lines for exit code 0; \"churn done\" is logged when the 25th is LAUNCHED, so wait for its reap)", wait_for(lambda: after_ready().count("exited code=0") >= 25, 90))
        lat = latency(4)
        check("the shell is responsive afterwards", all_changed(lat))
        check("no scheduler bug was reported", "KERNEL BUG" not in log())
    check("no panic or failed self-test at the end", "KERNEL PANIC" not in log() and "SELFTEST FAILED" not in log())
    print("shell_sched_check %s: %d passed, %d failed" % (tag.strip(), passed, failed))
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
