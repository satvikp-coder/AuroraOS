# Native graphics performance baseline (QEMU/TCG only)

Status: measured, issue #7 (stage G6). **These numbers describe QEMU's TCG emulation on one development PC. They say nothing about the Samsung Galaxy F04 or any real device.**

## Environment

- Host: 13th Gen Intel Core i7-1355U, 4 vCPUs visible to WSL2 (Debian-based `kali-linux`), other applications running (memory-constrained machine).
- QEMU 11.1.0, `-machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M`, TCG, `-display none`, `-device ramfb`.
- Guest MMU on; framebuffer 1024x768 XRGB8888, 3,145,728 bytes (768 pages) reserved from RAM.
- Timer: ARM generic counter (`CNTPCT_EL0` via `timer_count()`), `CNTFRQ_EL0` = 62,500,000 Hz (verified at run time and printed). `us = ticks * 1,000,000 / freq`, 64-bit integer.

## Method

`kernel/ui/gfx_bench.c` (build with `AURORA_BENCH=1`; run `tests/qemu/bench.sh`): 5 warm-up iterations then 40 timed iterations per case; one result line per case printed after timing (nothing is printed inside a timed region); median and p95 (sample at index ceil(0.95 n) - 1) from the sorted samples. Timed region is drawing into the surface only. `gfx_present` is timed separately (on ramfb it is a memory barrier; QEMU reads the framebuffer on its own schedule).

Distinct quantities, never to be confused:

1. **CPU drawing time** - this document's table (guest side).
2. **Backend presentation** - `gfx_present` cost: about 0 us (a barrier); QEMU's own display refresh is not measured.
3. **QMP screendump acquisition** - host side, `tests/qemu/latency.py`: median 20.8 ms, p95 26.5 ms for an unchanged screen (20 samples). It is the cost of the *test tool*, not of rendering.
4. **End-to-end interactive latency** - `latency.py`: Tab key sent through QMP until a screendump first differs: median 36.5 ms, p95 55.4 ms, min 15.4 ms, max 55.5 ms (20 samples). Includes the QMP round trip, virtio-input delivery, the guest's 10 ms polling tick, the redraw and one screendump, so it is an upper bound.

## Results (guest CPU time, microseconds; run A, full table)

| case | median | p95 | min | max |
|---|---|---|---|---|
| clear 1024x768 | 5066 | 6526 | 3686 | 8926 |
| fill rect 512x384 | 1505 | 2526 | 865 | 3578 |
| rounded rect 512x384 r=24 | 1296 | 1793 | 859 | 1907 |
| rounded rect + 2 px border | 2061 | 3592 | 1074 | 4837 |
| text, 20 lines x 66 chars (16 px) | 2148 | 3211 | 1934 | 3938 |
| present | 0 | 0 | 0 | 27 |
| scene, 10 controls, full frame | 5093 | 7693 | 3816 | 12287 |
| scene, 50 controls, full frame | 8265 | 12295 | 5746 | 14360 |
| scene, 100 controls, full frame | 11976 | 18309 | 8235 | 20972 |
| key event -> redraw -> present, 100 controls | 9138 | 15844 | 5692 | 20654 |
| key event -> redraw -> present, 10 controls | 4614 | 8846 | 2602 | 9686 |

Run-to-run variation is large because the host is shared: three further runs gave a 1024x768 clear median of 2.0 / 2.5 / 4.4 ms and a 100-control full frame of 6.8 / 7.2 / 7.8 ms (p95 14.7-17.9 ms). Treat every figure as +-2x. Outliers (max values) are host scheduling noise, not guest behaviour; none were discarded.

Damaged-region experiment (redraw one widget's 110x56 neighbourhood of the 100-control scene instead of the whole frame): median 48 / 55 / 115 us across three runs, i.e. about 100x cheaper than a full frame.

## Memory

- Framebuffer: 3,145,728 bytes (768 pages), reserved from the top of RAM before the MMU is set up.
- Scene state for 100 widgets + control array + labels + focus context: 9,648 bytes (static).

## Conclusions and limits

- Full-frame redraw of a 100-control screen is ~7-12 ms median (p95 up to ~18 ms) under TCG on this PC: acceptable for a Tier 0 bring-up on this emulator, but it is not evidence about phone hardware. Full redraw is the strategy used by the demo and by the planned screens; damage-rectangle redraw is a proven cheap optimisation (see above) to adopt when a screen needs it.
- Not measured: real hardware, cache effects (TCG has no caches), memory bandwidth of a real display controller, power.
- Not a CI gate: the sanity run in `bench.sh` only checks completeness and median <= p95.
