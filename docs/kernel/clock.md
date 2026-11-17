# Native Clock screen and timer toast (Cycle 19)

* Screen `SCR_CLOCK`, reached from Index ("clock"); Home is unchanged. SIDs 250-255 (presets 1/5/25 min, Start, Cancel, Home).
* The shell is the owner (pid 0, generation 1) of its own timer through `timersys_shell_*`; quota 2 like any owner; no process can be that owner. The screen never shows a wall-clock time: only uptime and the remaining time, both from the monotonic counter.
* `shell_tick(now_ms)` runs every loop iteration, so a timer completes, releases its slot and raises the toast whatever screen is showing. States: READY, RUNNING, DONE, CANCELLED, UNAVAILABLE (no service attached), ERROR (limit / refused / lost). A refused cancel or a service that forgets the timer is reported as an error, never as finished.
* Toast: "Timer finished", ink fill, topmost layer but only inside the header over the Horizon slot, clear of the uptime label, never focusable, 4000 ms on the monotonic counter, one slot.
* Every spoken-line change is logged as `shell: say ...`; the toast as `shell: toast on|off`.
* Tests: `tests/kernel-host/shell_clock_test.c` (47 checks, scale 75..200, both palettes, randomized), `tests/qemu/clock-tests.sh` (real keys/pointer, pixels, a real 1 minute timer finishing off-screen).
* Not here: Aura timer commands, sound, persistence (a timer does not survive a restart), an EL0 Clock client.
