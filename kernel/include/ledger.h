/*
 * kernel/include/ledger.h - the DURABLE Passport: permission grants that survive a restart, stored on AuroraFS as kernel-owned files.
 *
 * What is stored (all owned by the reserved principal PRINCIPAL_PASSPORT, permission owner-only, so no application can read, write or even see them):
 *   passport.v1  560 bytes: "LGPP", container version, COMMIT counter, image length, the Passport image (passport.h, FNV checked), CRC-32 of the container.
 *                Always written whole at its fixed size (AuroraFS never shrinks a file; a whole-file write is copy-on-write: the file is the old or the new
 *                content, never a mix).
 *   anchor.v1    32 bytes: "ANCH", version, the Passport commit counter it vouches for, reserved Trail fields (zero until Trail is persisted), CRC-32.
 *
 * Only DURABLE grants (generation 0: "this stable principal number") are stored. Instance-bound grants (a process generation) are RAM-only and never reach
 * the disk: generations restart every boot, so a stored one would be inherited by an unrelated process.
 *
 * COMMIT PROTOCOL (one mutation): copy the table, apply the change to the copy, write passport.v1 (commit N+1), READ IT BACK and compare, write anchor.v1
 * (N+1), READ IT BACK and compare, and only then replace the live table. The two files are NOT one atomic transaction; the order is chosen so every crash
 * point has a defined, fail-closed meaning:
 *   crash before passport.v1 is written      -> disk = state N,   anchor = N      : the change is definitely NOT committed.
 *   crash after passport.v1, before anchor   -> disk = state N+1, anchor = N      : the change IS committed. The next boot sees "one ahead", verifies the
 *                                                                                     file and repairs the anchor. The caller was never told OK (outcome unknown
 *                                                                                     to it); the disk is authoritative after the restart.
 *   crash after the anchor                   -> disk = state N+1, anchor = N+1    : committed.
 *   a write that returns FS_EUNCERTAIN / an I/O error / a failed read-back          -> OUTCOME UNKNOWN: the live table is NOT changed, the ledger becomes
 *                                                                                     UNHEALTHY and stays so until the next boot reloads the disk, which is then
 *                                                                                     authoritative. A definite refusal (no space, quota, read-only) changes nothing
 *                                                                                     and leaves the ledger healthy.
 * At load: commit == anchor -> healthy; commit == anchor + 1 -> repair the anchor; anything else (the file is behind or far ahead of the anchor, a missing
 * counterpart) -> ROLLBACK DETECTED: the table is EMPTY (everything denied), the files are left untouched as evidence, mutations are refused.
 *
 * WHAT THIS DOES NOT PROTECT: every checksum here (FNV, CRC-32) detects accidents, not a deliberate rewrite, and the anchor lives on the same writable disk.
 * Someone who can write the disk can replace passport.v1 and anchor.v1 together with an older, consistent pair and nothing here can tell (QEMU virt has no
 * hardware monotonic counter or secure storage). What IS detected: a single file restored, deleted, truncated, damaged or edited, and the crash windows above.
 *
 * Enforcement rule for callers: ledger_passport_ok() is 1 only for a fully verified, writable ledger. Anything else (no disk, read-only, corrupt, newer
 * version, rollback, runtime failure) is 0, and an action that needs Passport must not run.
 */
#ifndef AURORA_LEDGER_H
#define AURORA_LEDGER_H
#include <stdint.h>
#include <stddef.h>
#include <passport.h>
#include <trail.h>

#define LEDGER_CONTAINER_VERSION 1
#define LEDGER_PASSPORT_FILE "passport.v1"
#define LEDGER_ANCHOR_FILE "anchor.v1"
#define LEDGER_PASSPORT_BYTES 560
#define LEDGER_ANCHOR_BYTES 32
#define LEDGER_TRAIL_FILE "trail.v1"
#define LEDGER_TRAIL_BYTES 2000                   /* "LGTR", version, record count, image length, the Trail image (trail.h), pad, CRC-32; always written whole */

enum ledger_state {
    LG_NODISK = 0,     /* no filesystem: RAM-only table, nothing durable, protected actions unavailable */
    LG_OK,             /* loaded, verified and writable */
    LG_RDONLY,         /* loaded and verified, but the disk refuses writes: grants/revocations refused, protected actions unavailable */
    LG_CORRUPT,        /* a ledger file is damaged or unparsable: table empty (deny all), files untouched */
    LG_NEWER,          /* a ledger file of a NEWER container version: table empty, files untouched, never overwritten by this code */
    LG_ROLLBACK,       /* the files disagree with each other beyond one repairable step: table empty, files untouched */
    LG_FAILED,         /* a runtime store failure or an unknown outcome: table frozen at the last verified state, mutations refused until the next boot */
    LG_HELD            /* this ledger is fine, but the OTHER one is damaged / newer / rolled back: no write of any kind happens so the evidence stays byte-for-byte intact */
};
enum ledger_result {
    LG_E_OK = 0, LG_E_STATE = -1, LG_E_PASSPORT = -2, LG_E_NOSPACE = -3, LG_E_RDONLY = -4, LG_E_UNKNOWN = -5, LG_E_ARG = -6
};

/* The filesystem as the ledger sees it (the kernel binds it to AuroraFS; tests bind a fake with fault injection). All errors are the FS_E* codes of fs.h
 * (negative); FS_EUNCERTAIN means the durability of the last write is unknown. Files are addressed by an opaque id from lookup/create. */
struct lg_io {
    int (*mounted)(void);
    int (*readonly)(void);                                                          /* 1 if the medium refuses writes */
    int (*lookup)(uint32_t owner, const char *name, uint64_t *id);                  /* FS_OK, FS_ENOENT or another error; owner = PRINCIPAL_PASSPORT or PRINCIPAL_TRAIL */
    int (*create)(uint32_t owner, const char *name, uint64_t *id);                  /* owner-only permissions */
    int (*size)(uint64_t id, uint32_t *size);
    int (*read)(uint64_t id, void *buf, uint32_t len);                              /* bytes read, or an error */
    int (*write)(uint32_t owner, uint64_t id, const void *buf, uint32_t len);       /* bytes written (== len), or an error; written AS the owner */
};

/* Load and verify the ledger. Call once after the filesystem is mounted (and again after a remount). Always leaves a defined state. */
int  ledger_attach(const struct lg_io *io);
int  ledger_state(void);                             /* the PASSPORT ledger's state */
int  ledger_trail_state(void);                       /* the TRAIL ledger's state (same enum) */
const char *ledger_state_name(int state);
int  ledger_passport_ok(void);                       /* 1 only for LG_OK */
uint32_t ledger_commit(void);                        /* the last verified commit counter (0 = none yet) */

/* The live table (read only for callers). Empty and denying whenever the ledger could not be verified. */
const struct passport *ledger_passport(void);
int  ledger_peek(uint32_t principal, uint64_t gen, uint8_t perm);   /* passport_peek on the live table, no audit record; instance-bound grants included */

/* Durable mutations by KERNEL code. They return LG_E_OK only after the change is verified on disk (read back) AND in the anchor. */
int  ledger_grant(uint32_t principal, uint8_t perm);
int  ledger_revoke(uint32_t principal, uint8_t perm);

/* Trail (trail.v1, owner PRINCIPAL_TRAIL). Same protocol: the whole file (record count capped at TR_MAX, oldest evicted) is written and read back, then the anchor
 * (which now also vouches for the number of records ever appended and the chain head), then the live Trail is replaced. LG_E_OK means the record is on the disk. */
int  ledger_trail_ok(void);                          /* 1 only for LG_OK: verified, writable, not held for evidence */
const struct trail *ledger_trail(void);              /* the live chain (a damaged one keeps its verified prefix for display; appends are refused) */
uint32_t ledger_trail_commit(void);                  /* records ever appended = next sequence - 1 */
int  ledger_trail_append(const struct trail_rec *in);
int  ledger_ok(void);                                /* both ledgers OK: what an action that needs Passport AND Trail checks */

/* RAM-only, instance-bound grants (never stored, never counted in the commit): for the broker's process-bound rights. */
int  ledger_grant_instance(uint32_t principal, uint64_t gen, uint8_t perm);
int  ledger_drop_instance(uint64_t gen);

/* Kernel-initiated, AUDITED durable changes: the Passport commit as above, then a Trail record (kind 12 grant / 13 revoke, target = permission, requester = the
 * principal concerned, outcome done). The grant stands even if the Trail cannot record it (the rc of the audit is logged); an action that MUST be recorded
 * is a different thing (S1 and above append FIRST and run only if the append succeeded). */
int  ledger_grant_audited(uint32_t principal, uint8_t perm);
int  ledger_revoke_audited(uint32_t principal, uint8_t perm);

/* Kernel boot hook (ledger_kernel.c): attach to AuroraFS as PRINCIPAL_PASSPORT, log the verified state. Test builds (AURORA_LEDGER_TEST=n) then run a fixed script. */
void ledger_boot(void);
void ledger_set_boot_hook(void (*fn)(void));    /* called inside ledger_boot, after the ledgers are verified and BEFORE the boot marker is appended (recovery looks at the newest record) */

/* Diagnostics */
int  ledger_last_fs_error(void);                     /* the FS_E* behind the last non-OK result (0 if none) */
#endif
