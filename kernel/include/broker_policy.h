/*
 * kernel/include/broker_policy.h - the Native Action Broker's POLICY: a pure decision function (issue #10, docs/aura/action-broker-design.md).
 *
 * decide(env, replay, request) answers one question: what may happen to this validated intent, asked by this kernel-attested requester? It executes
 * nothing, draws nothing and holds no capability. Its inputs, and where each comes from:
 *   - the intent: untrusted DATA (typed text or, later, a model). It is re-validated here with aura_validate() and its risk class is NEVER trusted: the
 *     policy derives the class from the intent KIND with its own table.
 *   - the requester: only (pid, generation) travel with the request, and both come from the kernel (the syscall caller or the shell itself). The rights
 *     and principal are looked up in the kernel's live table through env->lookup; nothing in the request can name a principal or a right.
 *   - the environment: which platform services really exist (navigation, trusted review path, Passport, Trail, hold-to-confirm input) and what the
 *     platform provides (aura_caps). A missing service means UNAVAILABLE, never a weaker substitute.
 * Decisions:  ALLOW_NOW (an S0 navigation by a requester holding the navigation right), REVIEW (S1, needs the trusted review path AND the right),
 *             HOLD (S3, needs review, Passport, Trail and hold-to-confirm input), UNAVAILABLE (a dependency or service is missing), DENY (invalid,
 *             stale, forged, replayed, unauthorised or not actionable). Every decision other than ALLOW_NOW means NOTHING happens yet; ALLOW_NOW is
 *             permission for exactly one narrow executor, not a command.
 * The only state it writes is the replay table, and only after the requester's identity has been verified.
 */
#ifndef AURORA_BROKER_POLICY_H
#define AURORA_BROKER_POLICY_H
#include <stdint.h>
#include <aura_cmd.h>

enum bp_decision { BP_DENY = 0, BP_ALLOW_NOW, BP_REVIEW, BP_HOLD, BP_UNAVAILABLE, BP_COUNT };
enum bp_reason {
    BPR_NONE = 0,
    BPR_BAD_INTENT,          /* aura_validate() refused it (also: a forged risk class, wrong schema version, out-of-range value) */
    BPR_BAD_REQUESTER,       /* the requester is not a live process / the shell, or its identity record is inconsistent */
    BPR_STALE_REQUESTER,     /* pid is live but its generation differs: a stale identity (the pid was reused) */
    BPR_REPLAY,              /* the nonce was already used (or is not newer) for this requester instance */
    BPR_TABLE_FULL,          /* no room to track another requester's nonces: fail closed */
    BPR_NO_RIGHT,            /* the requester lacks the right this action needs */
    BPR_NOT_ACTIONABLE,      /* clarify, unknown, refused or rejected input: there is nothing to do */
    BPR_UNSUPPORTED,         /* no native service exists for it */
    BPR_NO_NAVIGATION,       /* the shell navigation service is not available */
    BPR_NO_REVIEW,           /* no trusted review path with a non-spoofable input path exists */
    BPR_NO_PASSPORT, BPR_NO_TRAIL, BPR_NO_HOLD,
    BPR_COUNT
};

/* Rights a requester can hold (granted by kernel code only; an EL0 client holds none by default). */
#define BP_RIGHT_NAVIGATE 0x1u     /* may cause S0 navigation in the shell */
#define BP_RIGHT_SETTINGS 0x2u     /* may propose an S1 settings change (still needs trusted review) */
#define BP_RIGHTS_ALL (BP_RIGHT_NAVIGATE | BP_RIGHT_SETTINGS)

#define BP_SHELL_PID 0u            /* the trusted shell itself is requester pid 0, generation 1 (it is not a process) */
#define BP_SHELL_GEN 1u

/* The kernel's view of a requester. Returns 1 and fills the outputs if (pid) is live now; 0 otherwise. */
typedef int (*bp_lookup_fn)(uint32_t pid, uint64_t *gen, uint32_t *principal, uint32_t *rights);

struct bp_env {
    const struct aura_caps *caps;
    bp_lookup_fn lookup;        /* NULL: no requester can be verified, everything is denied */
    uint8_t nav_service;        /* the shell navigation executor exists */
    uint8_t review_path;        /* a compositor-drawn review with a non-spoofable approval path exists */
    uint8_t passport, trail, hold_input;   /* the native Passport, the Trail and tactile hold-to-confirm: all absent today */
};

struct bp_request {
    uint32_t pid; uint64_t gen;  /* the requester as the KERNEL attested it */
    uint64_t nonce;              /* strictly increasing per requester instance, from 1 */
    struct aura_intent intent;
};

#define BP_REPLAY_SLOTS 9        /* eight process slots plus the shell */
struct bp_replay { uint32_t pid[BP_REPLAY_SLOTS]; uint64_t gen[BP_REPLAY_SLOTS]; uint64_t last[BP_REPLAY_SLOTS]; uint8_t used[BP_REPLAY_SLOTS]; };

struct bp_result { uint8_t decision, reason, risk; uint32_t principal; };   /* risk: the class the POLICY derived; principal: the verified one */

void bp_replay_init(struct bp_replay *r);
void bp_decide(const struct bp_env *env, struct bp_replay *replay, const struct bp_request *req, struct bp_result *out);
/* The canonical risk class (0..3) of an intent kind, or 255 for a kind that is not an action. */
uint8_t bp_risk_of_kind(uint8_t kind);
const char *bp_decision_name(uint8_t d);
const char *bp_reason_name(uint8_t r);
#endif
