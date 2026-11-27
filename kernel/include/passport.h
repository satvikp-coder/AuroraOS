/*
 * kernel/include/passport.h - Passport foundation (issue #11): a bounded, fail-closed permission ledger with an audit ring and a versioned,
 * checksummed on-disk image. PURE: no allocation, no clock, no libc; storage is a callback so it is host-testable and failure-testable.
 *
 * What it is: a table of (principal, permission) grants decided and recorded by KERNEL code only. passport_check() answers allow or deny and says why;
 * the default for everything is deny. Every grant, revoke and refused attempt is appended to a bounded audit ring (sequence numbers, never a wall-clock
 * time: only a monotonic sequence exists).
 *
 * What it is not (yet): it is not wired into the Action Broker (broker env.passport stays 0), it does not yet protect any filesystem or settings path
 * (those keep their own capability checks, which this does not weaken), and no EL0 interface exists. A process can never grant itself or anyone a permission:
 * the mutating calls take a pp_actor, and only the KERNEL actor is accepted; a PROCESS actor is refused and the refusal is audited.
 *
 * Identity reuse: a grant is bound to a principal NUMBER (the kernel's stable identity, not a pid) and may additionally be bound to a process generation;
 * a generation-bound grant stops applying the moment the instance is replaced. Generation-bound grants live in RAM ONLY: process generations restart at 1
 * every boot, so persisting one would let a later boot's unrelated process inherit it. The image therefore carries only gen == 0 grants, and a decoder refuses
 * an image that contains a bound one. The image checksum is unkeyed (FNV-1a): it detects accidental corruption, not a deliberate rewrite or rollback; the
 * caller must keep the store kernel-owned and keep a monotonic anchor outside the image.
 */
#ifndef AURORA_PASSPORT_H
#define AURORA_PASSPORT_H
#include <stdint.h>
#include <stddef.h>

#define PP_VERSION 1
#define PP_MAX_GRANTS 32
#define PP_AUDIT 32
#define PP_PER_PRINCIPAL 8              /* grants one principal may hold: a churning or hostile principal cannot fill the shared table */

enum pp_perm { PP_NONE = 0, PP_NAVIGATE, PP_SETTINGS_PROPOSE, PP_FILES_WRITE, PP_FILES_DELETE, PP_INSTALL, PP_SEND, PP_TIMER, PP_COUNT };
enum pp_actor_kind { PP_ACTOR_KERNEL = 1, PP_ACTOR_PROCESS = 2 };
struct pp_actor { uint8_t kind; uint32_t pid; uint64_t gen; uint32_t principal; };
enum pp_result { PP_OK = 0, PP_E_ACTOR = -1, PP_E_PERM = -2, PP_E_PRINCIPAL = -3, PP_E_FULL = -4, PP_E_NOENT = -5, PP_E_STORE = -6, PP_E_FORMAT = -7, PP_E_VERSION = -8, PP_E_ARG = -9, PP_E_QUOTA = -10 };
enum pp_op { PP_OP_GRANT = 1, PP_OP_REVOKE, PP_OP_CHECK_DENY, PP_OP_LOAD, PP_OP_SAVE };

struct pp_grant { uint32_t principal; uint64_t gen; uint8_t perm; uint8_t used; };   /* gen 0 = the principal in general, else only that process instance */
struct pp_audit_rec { uint32_t seq; uint8_t op, perm; int8_t result; uint8_t actor_kind; uint32_t actor_pid, principal; };

struct passport {
    struct pp_grant g[PP_MAX_GRANTS];
    struct pp_audit_rec log[PP_AUDIT]; uint32_t seq;          /* MUTATION ring (grant, revoke, load, save): next sequence number; holds the last PP_AUDIT records */
    struct pp_audit_rec dlog[PP_AUDIT]; uint32_t dseq;        /* DENIAL ring: denied checks go here, so a flood of denials can never evict a grant or revoke record */
    uint8_t loaded_ok;                                          /* 1 if the table came from a valid image (or was created empty on purpose) */
};

/* Storage is supplied by the caller: write the whole image / read it back. Return 0 on success, nonzero on failure. */
struct pp_store { int (*write)(const uint8_t *buf, size_t len); int (*read)(uint8_t *buf, size_t cap, size_t *len); };
#define PP_IMAGE_MAX (16 + PP_MAX_GRANTS * 16 + 8)

void passport_init(struct passport *p);                                                         /* empty table: everything denied */
int  passport_grant(struct passport *p, const struct pp_actor *by, uint32_t principal, uint64_t gen, uint8_t perm);
int  passport_revoke(struct passport *p, const struct pp_actor *by, uint32_t principal, uint8_t perm);
/* Revoke ALL grants (general and generation-bound) for a principal; returns count revoked or negative PP_E_*. Only PP_ACTOR_KERNEL may call. */
int  passport_revoke_all(struct passport *p, const struct pp_actor *by, uint32_t principal);
int  passport_count_grants(const struct passport *p, uint32_t principal);
int  passport_has_any(const struct passport *p, uint32_t principal);
/* 1 = allowed. Principal 0 and PRINCIPAL_NONE (0xFFFFFFFF) are never allowed. A generation-bound grant applies only to that generation. */
int  passport_check(struct passport *p, uint32_t principal, uint64_t gen, uint8_t perm);
/* Same decision as passport_check but with NO audit record (for internal lookups that must not flood the denial ring). */
int  passport_peek(const struct passport *p, uint32_t principal, uint64_t gen, uint8_t perm);
/* Drop every grant bound to this process generation (the instance is gone). Returns how many were dropped. */
int  passport_drop_instance(struct passport *p, uint64_t gen);
uint32_t passport_denial_count(const struct passport *p);
const struct pp_audit_rec *passport_denial_at(const struct passport *p, uint32_t i);   /* 0 = oldest held */
uint32_t passport_audit_count(const struct passport *p);                                       /* records currently held (<= PP_AUDIT) */
const struct pp_audit_rec *passport_audit_at(const struct passport *p, uint32_t i);           /* 0 = oldest held */

size_t passport_encode(const struct passport *p, uint8_t *buf, size_t cap);                    /* the versioned, checksummed image; 0 if cap is too small */
int  passport_decode(struct passport *p, const uint8_t *buf, size_t len);                      /* PP_OK, or an error and an EMPTY table (fail closed) */
int  passport_save(struct passport *p, const struct pp_store *st);
int  passport_load(struct passport *p, const struct pp_store *st);                             /* any failure leaves an empty table and loaded_ok = 0 */
#endif
