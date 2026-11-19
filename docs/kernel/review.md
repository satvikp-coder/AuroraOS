# Trusted review state machine (Cycle 20, Plan C1)

`kernel/ui/review.c` + `kernel/include/review.h`: the pure rules for "a person must look at exactly this change and approve it". It draws nothing, reads no input and changes no setting. **S1 execution stays disabled** (`env.review_path` is still 0): the compositor modal, the secure-attention routing and the S1 executor (Plan C2..C4) do not exist yet.

* One review at a time (`RV_E_BUSY`). Tickets strictly increase and are never reused; declined, expired, abandoned and consumed tickets are retired for good, so an old approval cannot be replayed.
* Only a validated S1 settings change can be opened (`aura_validate` + kind `AK_SET_SETTING`, status needs-approval, risk 1) and it is re-validated at execution.
* The approval is bound to the digest (FNV-1a 64 over an explicit little-endian serialisation) of every intent field (including all 48 preview bytes), the requester's pid, process generation, principal and nonce, and the previous and proposed values. `rv_verify` recomputes it from values re-read NOW and refuses on any difference.
* Timing (monotonic ms from the caller): the secure-attention gesture arms only >= 600 ms after opening (a held key or an immediate software gesture is a no-op); Approve works only < 10 s after arming (then it falls back to OPEN and says LAPSED); the review expires at 60 s; an approval nobody executes expires after 10 s. A clock that goes backwards counts as no time passed.
* `rv_approve` moves to APPROVED; `rv_verify` consumes it (DONE) exactly once; any failed verify retires the ticket. The requester exiting (or its generation being replaced) kills the review.
* **Limits:** the module cannot tell a person from software sending the same gesture. Trust rests on the compositor routing the gesture here only from the kernel's own keyboard path and never to a client (Plan C2); under QEMU the host/QMP can inject that gesture, so nothing here protects against a malicious host controlling the emulator.
* Tests: `tests/kernel-host/review_test.c` (49 checks incl. a 30,000-step randomized run against independently tracked invariants; 17 targeted mutants killed).
