#!/usr/bin/env python3
"""tests/qemu/latency.py - HOST-SIDE end-to-end interactive latency under QEMU/TCG (not the renderer's frame time).
Measures, with time.monotonic() on the host: (a) the cost of one QMP screendump of an unchanged screen (acquisition overhead), and
(b) the time from sending a Tab key through QMP until a screendump first differs from the previous one. (b) INCLUDES QMP round trip,
virtio-input delivery, guest polling (<=10 ms timer tick), the guest redraw and one screendump, so it is an upper bound and
must not be read as frame time. Output: medians/p95 in ms. Usage: latency.py --qmp SOCK --serial LOG --tmp DIR [--n 20]"""
import argparse, os, statistics, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_check import Qmp, read_ppm, wait_serial

def pct(v, p): v = sorted(v); return v[min(len(v) - 1, max(0, -(-len(v) * p // 100) - 1))]

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--qmp", required=True); ap.add_argument("--serial", required=True)
    ap.add_argument("--tmp", required=True); ap.add_argument("--n", type=int, default=20); a = ap.parse_args()
    q = Qmp(a.qmp, time.time() + 120)
    if not wait_serial(a.serial, "ui: ready", time.time() + 120): print("latency: guest never ready"); return 1
    path = os.path.join(a.tmp, "lat.ppm")
    def grab(): q.cmd("screendump", filename=path, format="ppm"); return read_ppm(path)[2]
    def key(d): return {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": "tab"}}}
    base = grab(); dump = []
    for _ in range(a.n):
        t = time.monotonic(); grab(); dump.append((time.monotonic() - t) * 1000)
    e2e = []
    for _ in range(a.n):
        prev = grab(); t = time.monotonic()
        q.cmd("input-send-event", events=[key(True), key(False)])
        while True:
            cur = grab()
            if cur != prev: break
            if time.monotonic() - t > 5: print("latency: no change within 5 s"); return 1
        e2e.append((time.monotonic() - t) * 1000)
    print("latency: screendump_unchanged_ms median=%.1f p95=%.1f n=%d" % (statistics.median(dump), pct(dump, 95), len(dump)))
    print("latency: tab_to_changed_screendump_ms median=%.1f p95=%.1f min=%.1f max=%.1f n=%d" % (statistics.median(e2e), pct(e2e, 95), min(e2e), max(e2e), len(e2e)))
    return 0

if __name__ == "__main__": sys.exit(main())
