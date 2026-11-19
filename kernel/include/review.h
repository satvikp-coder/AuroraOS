/*
 * kernel/include/review.h - the trusted-review STATE MACHINE (Plan C1, Cycle 20): the pure rules for "a person must look at exactly this change and approve
 * it". Pure: no clock of its own (the caller passes monotonic milliseconds), no allocation, no libc, no I/O. It does not draw anything, read any input or
 * change any setting; the compositor modal, the secure-attention path and the S1 executor (Plan C2..C4) are separate, later pieces and S1 execution stays
 * DISABLED (the broker's review_path is still 0) until they exist and are tested.
 *
 * What it guarantees (each is a test):
 *  - ONE review at a time. A ticket is a strictly increasing 64-bit number, never reused; a consumed, declined, expired or abandoned ticket is retired for
 *    good, so an old approval can never be replayed against a new review.
 *  - An approval is bound to EXACTLY ONE request: the validated intent (every field), the requester (pid + process generation + principal + nonce) and the
 *    previous and proposed values. rv_digest is an FNV-1a 64 over an explicit little-endian serialisation of all of it. At execution rv_verify recomputes the
 *    digest from the values as they are NOW (the caller re-reads the prior value) and refuses on any difference: a changed intent byte, another requester, a
 *    stale generation or a setting that changed meanwhile all fail.
 *  - Approval needs the review to be ARMED: the secure-attention gesture is accepted only RV_ARM_DELAY_MS (600 ms) after the review opened (a gesture
 *    that was already being held, or sent at once by software, is a no-op), and Approve works only inside the RV_APPROVE_WINDOW_MS (10 s) after arming. Arming
 *    never approves. A review expires RV_TIMEOUT_MS (60 s) after it opened.
 *  - Approval is consumed BEFORE rv_approve returns the proof, and rv_verify can be passed only once: the effect can run at most once per approval.
 *  - If the requester exits (or its generation is replaced) the review is dead; a dead review cannot be approved and its ticket is retired.
 *  - Time that goes backwards counts as no time having passed (it can never make a window longer, shorter or reopen one).
 *
 * What it is NOT: it cannot tell a person from software that sends the same gesture. The trust rests on the compositor routing the gesture to this module
 * only from the keyboard path the kernel owns, never to a client (Plan C2), and under QEMU the host/QMP can inject that gesture, so none of this protects
 * against a malicious host controlling the emulator.
 */
#ifndef AURORA_REVIEW_H
#define AURORA_REVIEW_H
#include <stdint.h>
#include <aura_cmd.h>

#define RV_ARM_DELAY_MS 600u
#define RV_APPROVE_WINDOW_MS 10000u
#define RV_TIMEOUT_MS 60000u

enum rv_state { RV_IDLE = 0, RV_OPEN, RV_ARMED, RV_APPROVED, RV_DECLINED, RV_EXPIRED, RV_GONE, RV_DONE };
enum rv_result { RV_OK = 0, RV_E_ARG = -1, RV_E_BUSY = -2, RV_E_STATE = -3, RV_E_TICKET = -4, RV_E_EARLY = -5, RV_E_LAPSED = -6, RV_E_CHANGED = -7, RV_E_INTENT = -8 };

/* Everything an approval is bound to. The caller fills it from the kernel's own records, never from client words. */
struct rv_request {
    struct aura_intent intent;       /* the validated S1 intent (aura_validate must accept it) */
    uint32_t pid; uint64_t gen;      /* the requester as the KERNEL attested it */
    uint32_t principal;              /* its kernel-assigned identity */
    uint64_t nonce;                  /* the broker nonce of the request */
    int32_t prior, next;             /* the setting's value before and after, as the shell read them */
};

struct rv {
    uint8_t state;
    uint64_t ticket;                 /* the live ticket, 0 when none */
    uint64_t last_ticket;            /* the highest ticket ever issued: tickets only grow */
    uint64_t digest;                 /* of the request the live ticket was opened for */
    uint32_t pid; uint64_t gen;      /* the requester, for rv_requester_gone */
    uint64_t t_open, t_armed, t_approved, last_now;
    uint32_t anomalies;              /* times the clock went backwards */
};

void rv_init(struct rv *r);
/* FNV-1a 64 over the explicit little-endian serialisation of every field of the request (exposed for the tests). */
uint64_t rv_digest(const struct rv_request *q);
/* Open a review for q. Returns the ticket (> 0) or a negative RV_E_*: RV_E_BUSY while another is OPEN/ARMED/APPROVED, RV_E_INTENT if the intent is not a valid
 * S1 settings change for these caps, RV_E_ARG for NULL/zero identities. A finished review (declined, expired, gone, done) is replaced. */
int64_t rv_open(struct rv *r, uint64_t now_ms, const struct rv_request *q, const struct aura_caps *caps);
/* Advance time: lapses an arming after 10 s (back to OPEN), expires the review after 60 s, drops an unverified approval after 10 s. Returns the state. */
int rv_tick(struct rv *r, uint64_t now_ms);
/* The secure-attention gesture. repeat != 0 (a held key) is ignored. Arms an OPEN review (or re-arms after a lapse) if >= 600 ms have passed since it opened.
 * Returns RV_OK when armed now, RV_E_EARLY (too soon, nothing changes), RV_E_STATE (no live review). */
int rv_attention(struct rv *r, uint64_t now_ms, int repeat);
/* The person chose Approve on THIS ticket. Valid only when ARMED and inside the approval window. RV_OK moves to APPROVED; the proof is consumed by rv_verify. */
int rv_approve(struct rv *r, uint64_t now_ms, uint64_t ticket);
int rv_decline(struct rv *r, uint64_t ticket);                    /* Cancel / Esc: any live state; the ticket is retired */
int rv_requester_gone(struct rv *r, uint32_t pid, uint64_t gen);  /* 1 if that requester's live review died */
/* At execution: the review must be APPROVED for this ticket and the request as it is NOW must digest to what was approved and still validate. RV_OK consumes
 * the approval (state DONE): a second call is RV_E_STATE. Any failure retires the ticket and moves to DECLINED (nothing may run). */
int rv_verify(struct rv *r, uint64_t now_ms, uint64_t ticket, const struct rv_request *now_q, const struct aura_caps *caps);
const char *rv_state_name(int state);
#endif
