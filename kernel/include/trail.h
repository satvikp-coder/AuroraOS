/*
 * kernel/include/trail.h - Trail foundation (issue #11): a bounded, durable-by-design record of what the Action Broker decided and what REALLY happened.
 * PURE: no allocation, no clock, no libc; storage is a callback so restart, torn writes and failures are testable on the host.
 *
 * Each record carries a sequence number, the kernel-attested requester (pid + principal), the canonical action (kind, target, argument), the policy's
 * decision and reason, the ACTUAL execution result and a monotonic UPTIME in milliseconds. It never carries a wall-clock time (none exists: there is no
 * RTC service) and never carries user-typed text. Records are hash-chained (FNV-1a 64 over the previous chain value and the record) so reordering,
 * dropping, editing or splicing is detected; the oldest records may be evicted from the bounded ring, and the chain value before the first retained
 * record is kept (base_chain) so what remains still verifies.
 *
 * Fail-closed rule for callers: an action that must be recorded (S1 and above) must call trail_append() FIRST and may run only if it returns TR_OK. If the
 * store fails the record is NOT committed in memory either, so memory and disk never disagree about what happened.
 *
 * What it is not (yet): nothing writes to it, it is not wired to AuroraFS, and there is NO Undo: a receipt is evidence of what happened, not a way to
 * reverse it. Undo needs a real reversible operation plus the prior value, neither of which exists.
 */
#ifndef AURORA_TRAIL_H
#define AURORA_TRAIL_H
#include <stdint.h>
#include <stddef.h>

#define TR_VERSION 1
#define TR_MAX 48                      /* records retained */
#define TR_REC_BYTES 40
#define TR_IMAGE_MAX (24 + TR_MAX * TR_REC_BYTES + 4)

enum { TR_OK = 0, TR_E_ARG = -1, TR_E_STORE = -2, TR_E_FORMAT = -3, TR_E_VERSION = -4, TR_E_CHAIN = -5 };

struct trail_rec {
    uint32_t seq;                      /* assigned by trail_append; strictly increasing, never reused */
    uint32_t pid, principal;           /* the requester as the KERNEL attested it */
    uint8_t  kind, target, risk;       /* canonical action (aura_kind, screen/space/setting id, risk class 0..3) */
    uint8_t  decision, reason, state;  /* policy decision, policy reason, actual outcome (AX_*) */
    int32_t  arg;
    uint32_t uptime_ms;                /* monotonic uptime, NOT wall-clock time */
    uint64_t chain;                    /* assigned by trail_append */
};

struct trail {
    struct trail_rec r[TR_MAX]; uint32_t n;          /* oldest first */
    uint32_t next_seq; uint64_t base_chain;           /* chain value before r[0] */
    uint32_t evicted;                                  /* records dropped from the ring over its life (information only) */
    uint8_t healthy;                                   /* 0 after a decode that found damage: the retained prefix is still valid, appends are refused */
    uint32_t recovered, lost;                          /* set by trail_decode: records kept / records beyond the damage */
};

struct tr_store { int (*write)(const uint8_t *buf, size_t len); int (*read)(uint8_t *buf, size_t cap, size_t *len); };

void trail_init(struct trail *t);
/* Validate the caller's fields, assign seq and chain, persist the WHOLE new image through the store, and only then commit in memory. */
int  trail_append(struct trail *t, const struct tr_store *st, const struct trail_rec *in);
int  trail_verify(const struct trail *t);                                  /* TR_OK, or TR_E_CHAIN / TR_E_FORMAT if any retained record does not verify */
size_t trail_encode(const struct trail *t, uint8_t *buf, size_t cap);
/* Decode an image. On damage the longest verified prefix is kept (recovered), the rest counted as lost, healthy = 0 and TR_E_CHAIN/TR_E_FORMAT is returned;
 * a wrong magic, version or length leaves an empty, unhealthy trail. A clean image returns TR_OK with healthy = 1. */
int  trail_decode(struct trail *t, const uint8_t *buf, size_t len);
int  trail_load(struct trail *t, const struct tr_store *st);
#endif
