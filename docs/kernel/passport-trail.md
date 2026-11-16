# Passport and Trail foundations (issue #11)

Status: **pure, host-tested building blocks. Nothing is wired to them yet.** The Action Broker's `env.passport` and `env.trail` flags stay 0, so every S2/S3 path remains UNAVAILABLE. Issue #11 stays open: there is no enforcement, no AuroraFS persistence and no recovery in a running kernel yet. QEMU only.

## Passport (`kernel/ui/passport.c`)
A bounded permission ledger (32 grants) of `(principal, optional generation, permission)` with a 32-record audit ring and a versioned, checksummed image.
* **Default deny.** Principal 0 (the kernel) and `PRINCIPAL_NONE` are never allowed; unknown permissions are denied.
* **Kernel-only mutation.** `passport_grant`/`passport_revoke` take an actor and accept only the KERNEL actor. A PROCESS actor (any EL0 process) is refused with `PP_E_ACTOR` and the refusal is audited, so no process can grant itself or anyone a permission.
* **Identity reuse.** A grant may be bound to a process generation; it stops applying when the instance is replaced, and an unknown generation never matches a bound grant. Revoking removes every generation of that permission for the principal.
* **Audit.** Grants, refused grants, revokes and DENIED checks get monotonically increasing sequence numbers (allowed checks are not logged, they would flood the ring). There is no wall-clock time.
* **Persistence (planned, not wired).** Image `PPRT`, version 1, FNV-1a checksum; any bit flip, truncation, trailing byte, unknown version, duplicate record or invalid record yields an EMPTY, denying table. Storage is a callback so write/read failures and torn writes are tested.

## Trail (`kernel/ui/trail.c`)
A bounded (48 records) hash-chained record of what the broker decided and what REALLY happened: sequence, requester pid/principal, action kind/target/risk/argument, decision, reason, actual outcome, and monotonic uptime in milliseconds (never wall-clock time, never user text).
* **Commit only after the store.** `trail_append` writes the whole new image first and commits in memory only on success; a failed write leaves memory unchanged and returns `TR_E_STORE`. Rule for callers: an S1+ action must run only after `trail_append` returned OK.
* **Integrity.** The chain detects edits, swaps, deletions, sequence jumps and a dropped newest record. Evicting the oldest records keeps the chain value before the first retained one, so the rest still verifies.
* **Recovery.** A damaged image keeps the longest verified prefix (`recovered`/`lost`), marks the trail unhealthy and refuses further appends. A torn or wrong-length image, wrong magic or unknown version leaves an empty unhealthy trail.
* **No Undo.** A receipt is evidence, not a way to reverse anything. Undo needs a real reversible operation and the prior value; neither exists.

## Not done
Wiring both to AuroraFS (quota and failure semantics), per-principal enforcement points, a trusted approval path and hold-to-confirm input, and boot-time recovery. Tests: `tests/kernel-host/passport_test.c` (77 checks incl. every single-bit flip and truncation of the image and a 30,000-step model check) and `trail_test.c`.
