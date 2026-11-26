# APP-02 — Stable app-state serialization

## Goal

Bounded, deterministic save/restore of an application's runtime state, owned by the application's kernel-assigned principal. State is persisted to the application's own private files on AuroraFS; no other identity can read or write it. The design is intentionally small: a fixed number of slots, a fixed maximum size per slot, and CRC-verified round-trip reads.

## Scope

- One app identity = one set of state slots (files named `<app>.state.<n>`, `0 <= n < APP_STATE_SLOTS`, owner-privileged perm 3).
- Bounded: `APP_STATE_SLOTS` slots (default 2), each at most `APP_STATE_MAX` bytes (default 4096).
- A save writes to the slot that does NOT hold the current state (alternating, like Notes), reads it back through the filesystem, decodes and compares; only then is the save acknowledged.
- Format: magic, version, length, sequence, CRC-32, payload (all little-endian, bounded).

## Interface (kernel/include/appstate.h — proposed)

```c
#define APP_STATE_SLOTS 2
#define APP_STATE_MAX   4096

enum { AS_OK = 0, AS_E_NOSPACE, AS_E_CORRUPT, AS_E_NEWER, AS_E_TOOBIG, AS_E_NODISK };

/* Save `len` bytes of `buf` under application `app` (a stable APP_* id). Sequence is assigned
 * by the store. Returns AS_OK or AS_E*. */
int app_state_save(uint32_t app, const void *buf, uint32_t len, uint32_t *seq_out);

/* Restore the most recent valid state for `app` into `buf` (cap `cap`). Fills `seq_out` with
 * the restored sequence. Returns AS_OK, AS_E_NODISK (no state yet), or AS_E_CORRUPT. */
int app_state_restore(uint32_t app, void *buf, uint32_t cap, uint32_t *seq_out);

/* Discard all saved state for `app` (e.g. on uninstall). Returns AS_OK. */
int app_state_discard(uint32_t app);
```

## Properties

- **Atomic within a slot**: a torn write leaves the previous slot intact; the new slot is only adopted after a successful read-back and compare.
- **Bounded**: no allocation, no unbounded growth; a payload larger than APP_STATE_MAX is refused (AS_E_TOOBIG), never silently truncated.
- **Version-aware**: a state written by a NEWER version of the app is detected (AS_E_NEWER) and never silently adopted.
- **Principal-scoped**: files are created under the app's kernel-assigned identity; AuroraFS authorization prevents cross-app reads/writes.

## Relation to APP-03

APP-02 provides the save/restore primitives. APP-03 decides WHEN to restore (on restart) and how to handle clean exit vs fault.
