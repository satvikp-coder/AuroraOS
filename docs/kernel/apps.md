# Native application launcher and the Calculator (Cycle 21)

**What exists.** A bounded kernel registry (`kernel/proc/apps.c`, `kernel/include/apps.h`) of applications the shell can start on a person's request, a Calculator EL0 application (`tests/programs/calc.c`, pure arithmetic in `calc_core.h`), and an Index entry that launches it.

**Rules (all enforced in kernel code, tested):**
* Stable ids (`APP_CALCULATOR = 1`). The executable name, the identity (principal 601) and the UI capability come from the kernel table. The person, the shell UI and EL0 programs never name an executable or an identity; there is still **no system call that starts a program**.
* Launch uses the existing trusted loader (`launch_register` / `launch_app`): the executable must be INSTALLER-owned, not writable by others, CRC-verified and a valid ELF. No second loader. Nothing is parsed or executed from the person's input.
* One running instance, identified by (pid, generation); a reused pid never matches. A second launch while it runs is refused ("Already open. See Pages.") and creates nothing.
* When the process exits or faults the run loop reaps it: surfaces and queued events (`ui_sys_client_gone`), timers, Passport grants bound to the generation, a pending review, and the launcher's record (`apps_gone`) are dropped, so a relaunch is a fresh instance (QEMU test: display starts at 0, one new surface). The UI capability is a per-identity grant by kernel code and is re-derived from the table, not carried by the process; the Calculator holds no navigate, timer, settings or Passport right.
* Honest availability: listed "(not available yet)" unless the filesystem is mounted and the installer-owned `calc.elf` exists; a refused launch shows the kernel's reason in place of the Index hint.
* A launch is NOT a security decision about the program: it is unprivileged EL0 like every other client, confined by the existing UI protocol (surface quota, kernel placement, focus rules). Esc and Tab are never delivered to a client (they return focus to the shell), so the Calculator quits with **Q**.

**Calculator semantics.** Integers only, at most 9 typed digits, results limited to ±999,999,999,999 (else "Overflow"), division truncates toward zero, divide by zero is the "Divide by zero" error, left-to-right evaluation (2 + 3 × 4 = 20). Keys: digits (row or keypad), + - * /, Enter/=, Backspace, C or Delete (clear), Q (quit).

**Where the executable comes from.** The shipped kernel has no built-in programs; `calc.elf` must be installed on the AuroraFS disk by an installer step (the QEMU test provisions it with `mkafs ... installer 0x7 calc.elf`). Without it the entry is honestly unavailable. There is no packaging of the disk image in the repository yet.

**Evidence.** Host: `calc_test` (28 checks + 200,000 random sequences against a `__int128` oracle), `shell_apps_test` (21). QEMU `tests/qemu/app-tests.sh` on the shipped kernel: launch from Index, kernel-chosen identity, Pages lists it, surface placed by the compositor, real keys 1 2 + 8 Enter = 20, 1/0 error, double launch refused, Q exits, cleanup, fresh relaunch; missing executable and missing disk launch nothing. The host/QMP can inject keys (no claim of protection against a malicious host).
