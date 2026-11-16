# Timer service core (issue #9, Clock/Timer): pure foundation

Status: **a pure, host-tested core. It is not wired to a system call, an EL0 application or Aura, so there is NO working timer in AuroraOS yet and Aura still answers "no timer service".** QEMU only.

`kernel/ui/timersvc.c` is a bounded (4 timers) table of countdowns against a caller-supplied **monotonic millisecond counter**:
* **Absolute deadlines.** A timer stores `deadline = start + duration` (saturating). It is complete when the counter has reached the deadline, however late the caller polls; nothing accumulates ticks or deltas, and nothing depends on frame counts. A reader starved for an hour still sees DONE with 0 remaining; irregular polling cannot drift the remaining time (tested).
* **Backwards counter.** A smaller reading than the last one is treated as "no time passed" and counted (`backwards`), never as a negative duration or an early completion.
* **Limits refused, not clamped.** 1 second to 24 hours; generation 0 refused; the table is bounded (FULL).
* **Ownership.** Bound to the kernel-attested (pid, generation); a different process or a later instance of the same pid sees exactly "no such timer". Only the owner can cancel, query or release; a completed timer cannot be cancelled; `tsvc_owner_gone` drops an exiting instance's timers.
* **No notification, sound or persistence.** "Done" is a state the owner reads.

Tests: `tests/kernel-host/timersvc_test.c` (31 checks, including a 40,000-step model check with stale counter readings). To become real the service needs: a syscall (kernel-attested requester, capability), an EL0 Clock app using the UI protocol, QEMU tests, and only then Aura's `caps.timer` and the "start a 25-minute timer" command.
