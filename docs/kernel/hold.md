# Hold-to-confirm state machine (Cycle 21, Plan C5)

`kernel/ui/hold.c` + `kernel/include/hold.h`: a pure 1,500 ms hold on the monotonic counter. **Not connected to anything**: `env.hold_input` stays 0 and S3 operations (delete, send, install, share) stay unavailable. It is one prerequisite among several (trusted review, Passport, Trail, a service-specific enforcement point).

* Starts on a non-repeat Enter/Space key-down on the focused control or a pointer press inside it; a stuck key's auto-repeat can never start one; a second press neither restarts nor extends it.
* Confirms only in `hold_poll`: 1,499 ms never, 1,500 ms yes, exactly once per hold. Any cancel or release processed before that poll wins (fail closed).
* Cancels on release of the starting key/button, the pointer leaving (re-entering does not resume), any other key, Escape, a focus change or a modal change. Repeats are ignored. After a confirmation nothing can start again until the starting key/button is released.
* Progress for the UI is floor(elapsed / 375) of four segments (four after a confirmation). A backwards clock counts as no time.
* Tests: `tests/kernel-host/hold_test.c` (36 checks incl. a 60,000-event randomized run against an independent model; 15 mutants killed).
