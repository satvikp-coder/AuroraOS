# Native Pages (issue #9)

Pages is a **read-only, live view of the real process table** inside the native Aurora Glass shell. Nothing on it is simulated.

* **Data:** `procsnap_take()` (`kernel/proc/procsnap.c`) copies a bounded snapshot of up to `PROC_MAX` (8) processes: pid, generation, name (sanitised to printable ASCII), state (new, ready, running, blocked, exited, faulted), scheduler ticks, context switches in, yields, preemptions, owned page count and, for exited processes, the exit code. It never writes to a process, allocates, or exposes kernel/physical addresses, registers, principals, capabilities or IPC grants (host test: marker bytes planted in every private field never appear in the snapshot).
* **Identity:** a process is `(pid, gen)`; the pid is reusable, the generation is not, so a reused pid shows as a new process.
* **Screen:** reachable from the Home Pages tile, the Index ("pages"), and the dock Back history; a Home button returns. The shell re-reads the table four times a second while Pages is open and repaints only if something displayed changed. At text scales above 125 percent rows switch to a compact form.
* **What it is not:** there is no Kill, Pause or Resume (no secure service exists); no memory-usage or CPU-percentage figures (the kernel does not measure them; the page count is physical pages the address space owns).
* **Tests:** `procsnap_test` (21), `shell_pages_test` (42, includes 75-200 percent scale and high contrast layout bounds and a 4000-step churn), `tests/qemu/pages-tests.sh` (real QMP input, screendump pixels, guest process lines). QEMU/TCG only.
