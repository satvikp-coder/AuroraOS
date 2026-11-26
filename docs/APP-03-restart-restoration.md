# APP-03 — Restart restoration

## Goal

Restore an application's saved state when it is restarted (a new process instance, new generation) so the person does not lose work between launches. Restoration is bounded, honest, and never automatic for data the app did not explicitly save.

## Scope

- Triggered only by the kernel launcher when starting an app via `apps_launch()`, after the process is created but before its first UI frame.
- Restores the most recent valid saved state (from APP-02) if one exists and matches the app's identity and version.
- On a clean exit, state is preserved (the app saved it before quitting). On a fault, state from the last successful save is still available — the fault does not erase the disk.
- If no saved state exists, the app starts fresh (as today).

## Interface (kernel/include/appstate.h — proposed, extends APP-02)

```c
/* Called by the kernel launcher (apps_launch) before the app's first frame. Copies the restored
 * state into `out` (cap `cap`) and returns the number of bytes copied, or a negative error.
 * The app itself decides what to do with the bytes. */
int app_state_restore_on_launch(uint32_t app, void *out, uint32_t cap);

/* Hint from the launcher: the previous instance of `app` exited cleanly (1) or faulted (0).
 * Restoration behaves the same either way — the disk is the source of truth. */
void app_state_note_exit(uint32_t app, int clean);
```

## Properties

- **Generation-safe**: a restarted instance is a new (pid, generation). State is keyed by app identity, not pid, so a recycled pid never resurrects another instance's state.
- **No silent data loss**: restoration only reads; it never deletes. The previous state remains on disk until the app explicitly overwrites it (APP-02) or discards it.
- **Opt-in per app**: only apps that call the restore interface get restoration; apps that don't (e.g. Calculator, which has no persistent user state) start fresh.
- **Bounded**: total state per app is capped at APP_STATE_SLOTS * APP_STATE_MAX.

## Relation to APP-02

APP-02 defines the save/restore primitives and on-disk format. APP-03 wires restoration into the launch path so it happens automatically at startup.
