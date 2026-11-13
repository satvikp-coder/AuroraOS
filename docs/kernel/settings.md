# Native settings registry (issue #6, milestone E)

Status: implemented and tested in QEMU only. This is a **kernel service on AuroraFS**, not the Aurora Glass browser Settings prototype (which stores
nothing natively and never talks to the kernel). The values are stored **preferences**: there is no display, audio, radio or power driver, so no setting
here controls any hardware; they are what a future compositor or service would read.

## Settings

| id | name | type | range / values | default | read | write capability |
|---|---|---|---|---|---|---|
| 1 | theme | enum | 0 auto, 1 light, 2 dark | 0 | any identity | appearance |
| 2 | text scale | integer | 75..200 (percent) | 100 | any identity | appearance |
| 3 | reduced motion | boolean | 0, 1 | 0 | any identity | appearance |
| 4 | focus mode | enum | 0 off, 1 work, 2 rest, 3 sleep | 0 | any identity | attention |
| 5 | aura mode | enum | 0 off, 1 suggest, 2 act with review | 1 | any identity | **system** |
| 6 | notifications | enum | 0 all, 1 priority only, 2 none | 0 | any identity | attention |

Values are validated as **64-bit numbers** against the type and range: nothing is clamped, rounded or truncated (`2^32 + 2` is not theme 2).

## Authorization

Capabilities (`CAP_SET_READ`, `_APPEARANCE`, `_ATTENTION`, `_SYSTEM`) are held by **identities (principals)**, granted **only by kernel code**
(`settings_grant`). Every real identity may read (`CAP_SET_READ` by default); writing needs the capability named in the table. The kernel principal (0)
has all. A process without an identity gets `AE_EPERM`. `settings_grant` refuses the kernel principal, "none", the installer, the settings owner and
empty or unknown capability bits. The system capability is separate from appearance and attention (tested both ways), so an app that may change the
theme still cannot change the aura mode.

No system call grants, revokes or lists capabilities. An untrusted program holds whatever its registry identity holds: nothing beyond read unless the kernel
that launched it said otherwise.

## System call: `SYS_SETTINGS = 9`

`x0` = operation (1 `GET(id)` -> value, 2 `SET(id, value)` -> 0, 3 `RESET(id)` -> 0), `x1` = id, `x2` = value. Errors are `-200 + SETTINGS_E*`:
`-201` no such setting, `-202` invalid value, `-203` not permitted, `-204` could not be saved, `-205` saved file is from a newer version; an unknown
operation is `AE_EINVAL`. Syscall numbers 10..40 remain `-ENOSYS`.

## Persistence

One file, `settings.v1`, owned by the reserved principal `PRINCIPAL_SETTINGS` (`0x7F000002`, which no process can ever hold) with permission for the
owner only, so a program cannot find, read, write, chmod or delete it - its id is indistinguishable from a bad id (existence policy, storage.md).
Record (40 bytes, little-endian): `"ASET"` | version `u32` = 1 | count `u32` = 6 | six `u32` values by id | CRC-32 of the first 36 bytes. On load every
value is re-validated: a record with a good CRC but an out-of-range value is corrupt.

**Write path:** validate -> authorise -> encode the whole record -> `fs_write` (AuroraFS replaces a whole file copy-on-write: the file is the old record or
the new one, never a mix) -> **only then** update the RAM copy. If saving fails the caller gets `SETTINGS_ESTORE`, RAM keeps the old value and nothing says
"saved". If the outcome is uncertain (`FS_EUNCERTAIN`: the filesystem unmounts itself) the same applies; after a remount `settings_load()` shows what the
disk actually holds. Tested: a write that landed although it was reported as failed is **visible only after the reload** - the reload is the truth.

**Load outcomes:** valid -> RAM = file. **Missing** -> defaults (not an error). **Corrupt** (bad size, magic, count, CRC, an out-of-range value, or the
filesystem's own data check failing) -> defaults in RAM, and the next valid write **replaces** the broken record. **Version mismatch** -> defaults in
RAM but the file is **kept untouched** and every write (even the kernel's) is `SETTINGS_EVERSION`: an older kernel never overwrites a newer one's data.

**Quota:** the settings principal has the ordinary AuroraFS quota (8 files, 96 blocks); the record is one file and one block, updates need no new
file or block, and creating it when the principal is already at its file limit fails with `SETTINGS_ESTORE` / `FS_EQUOTA` (tested).

## Evidence (`tests/qemu/settings-tests.sh`, 74 checks, also in CI)

* **Persistence across emulator restarts** (three QEMU processes on one disk): boot 1 finds no record (defaults); an EL0 client loaded from the disk saves
  text scale 110, dark theme, reduced motion, focus rest, notifications priority; a **new QEMU process** loads exactly those values (log:
  `status=0 (loaded from disk) theme=2 text_scale=110 ...`), the client raises the text scale to 120, and so on to 130. The host also finds the `ASET`
  record in the image file.
* **Authorization** from EL0 and from the kernel API: a writer with appearance + attention can set those and is refused the system-only aura mode; a guest
  with no capability can read all six and is refused every set and reset; a system UI can set the aura mode and is refused appearance; the kernel
  principal can set anything; "none" is refused everything. Refused calls write nothing to the device (the device write counter is compared).
* **Validation:** unknown id, wrong type (a boolean 2, theme 3), out of range (text scale 74, 201, `-1`), oversized (`2^32 + 2`), unknown operation.
* **Corrupt** (flipped bit, out-of-range value behind a valid CRC, truncated, wrong magic, garbage, wrong count), **missing** and **newer-version** records.
* **Storage failure** (injected below the filesystem): rejected before reaching the device (definite, retry works); lost with unknown outcome; landed but
  reported as failed.
* The kernel with no disk runs no settings test and boots normally.

## Not done / limits

* QEMU only; one CPU; no hardware is driven by any setting.
* Capabilities are granted by kernel code at launch time; there is no user-facing policy, no per-app configuration file, no change notifications, no
  per-user profiles, no migration from other versions (a newer record is preserved but not read).
* Six fixed settings with a fixed schema compiled into the kernel; adding a setting changes the record version.
* AuroraFS offers CRC-32 only: tampering by someone who can write the disk image is not detected.
