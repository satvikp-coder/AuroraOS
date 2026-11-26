# APP-05 — Safe note restoration from version history

## Goal

Guarantee that a saved note is never destroyed by a failed, torn, or incompatible write. Notes uses two alternating slot files ("note.a", "note.b") and read-back verification so the previous note always survives.

## Scope

- On save, the buffer is encoded into the slot that does NOT hold the current note (alternating), then read back through the filesystem, decoded, and compared byte-for-byte with the editor buffer. Only then is the status "Saved" and the current slot/sequence updated.
- A damaged slot (bad magic, CRC, length, flags, or characters) is never opened and never overwritten: saving is switched off and the status explains why.
- A slot from a NEWER version (NOTE_E_NEWER) is never overwritten: saving is switched off and the status says the saved note is from a newer version.
- A slot that is empty (0 bytes, created but never written) is treated as absent.

## Interface (tests/programs/notes.c — existing, documented)

The Notes application (`tests/programs/notes.c`) implements the policy directly:

- `load()` — reads both slots, picks the valid one with the highest sequence, locks on NEWER or BAD, reports "Loaded"/"New note"/storage-unavailable.
- `save()` — writes to the alternate slot, read-back verifies, only then updates `cur_slot`/`cur_seq`/`dirty`; refuses when `locked`; reports the exact reason on any failure.
- `SLOT[2] = { "note.a", "note.b" }` — two owner-private files (perm 3), written alternately.
- `enum { SL_EMPTY, SL_VALID, SL_BAD, SL_NEWER }` — per-slot decode class.

## Properties

- **No single-point-of-failure**: a torn write to one slot leaves the other slot's good note intact.
- **Read-back verification**: a write is not trusted until it is read back through the filesystem and decoded identically.
- **Never overwrite damaged or newer**: a bad or newer slot is locked; saving stops until the person resolves it (reopen Notes).
- **Principal-private**: slot files are owned by Notes' kernel-assigned identity (602); AuroraFS prevents other identities from reading or writing them.

## Relation to APP-02 and APP-03

APP-05 is Notes' concrete instance of the general app-state serialization (APP-02) and restart restoration (APP-03): Notes saves its one note before quitting (clean exit) and restores the highest-sequence valid slot on launch. A fault does not erase the disk, so the last saved note survives.
