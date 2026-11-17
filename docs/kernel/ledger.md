# Durable Passport ledger (Cycle 20, milestone B)

Source: `kernel/include/ledger.h` (protocol and crash analysis), `kernel/ui/ledger_store.c` (pure, host tested), `kernel/ui/ledger_kernel.c` (AuroraFS binding).

* Files `passport.v1` (560 B) and `anchor.v1` (32 B), owner `PRINCIPAL_PASSPORT` (0x7F000004), owner-only permission. No system call can name that principal, so no process can read, write, list or see them. Durable grants name a stable application principal and generation 0; instance-bound grants stay in RAM and never reach the disk.
* One mutation = write `passport.v1` (commit N+1) -> read back and compare -> write `anchor.v1` -> read back and compare -> replace the live table. OK is returned only then.
* Crash points (proved by the host test, which cuts the power before and after every one of the writes of a mutation script): before the passport write = not committed; between the files = committed, repaired at the next boot; after the anchor = committed. After any crash the table is exactly the state before or after the interrupted mutation.
* Unknown outcomes (FS_EUNCERTAIN, I/O errors, a failed read-back) freeze the ledger (`failed`): no mutation, no durable grant honoured, until the next boot reloads the disk, which is then authoritative. Definite refusals (no space, quota) change nothing and leave it healthy.
* At load: commit == anchor -> ok; passport exactly one ahead -> repair the anchor (deferred on a read-only disk); anything else -> `rollback-detected`; damaged -> `corrupt`; a newer container version -> `newer-version`. In all three the table is EMPTY (everything denied), mutations are refused and the files are never touched. Empty shells left by a creation that died inside its first write are treated as no ledger.
* `ledger_passport_ok()` is 1 only for `ok` (verified and writable). No disk / read-only / any failure => protected (S1+) actions must not run. RAM-only instance rights and the S0 paths are unaffected.

**Limits (stated plainly):** FNV and CRC-32 detect accidents, not a deliberate rewrite. The anchor is on the same writable disk, so an attacker who can write the disk can replace both files with an older consistent pair undetected (QEMU virt has no hardware counter or secure storage). What is detected: one file restored, deleted, truncated, damaged or edited, and every crash window above.

**Not in this change:** the broker does not consult the ledger yet (milestone D), Trail is not persisted (milestone C; the anchor's Trail fields are reserved zero).
