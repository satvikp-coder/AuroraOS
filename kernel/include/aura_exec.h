/*
 * kernel/include/aura_exec.h - the S0 Aura executor (issue #10): the narrow, verified bridge from an authorised intent to a real shell navigation.
 *
 *   text -> aura_parse() -> aura_validate() -> bp_decide() (requester authorisation) -> ALLOW_NOW -> aura_execute_s0() -> shell_go()/shell_select_space() -> verified
 *
 * aura_execute_s0 handles navigation, the shell timer and opening installed Notes through the shell's trusted launcher.
 * The application identity is fixed by the registry. Launch and timer intents are reserved to the shell at the broker and refused by the queued client path.
 * The executor revalidates the intent and reports success only when the requested effect is verified.
 * S1 and above never reach it: the policy answers REVIEW, HOLD or UNAVAILABLE and the outcome says nothing was changed.
 */
#ifndef AURORA_AURA_EXEC_H
#define AURORA_AURA_EXEC_H
#include <broker_policy.h>
#include <shell.h>
#include <trail.h>

enum { AX_NONE = 0, AX_DONE, AX_DENIED, AX_UNAVAILABLE, AX_NEEDS_APPROVAL, AX_NOT_UNDERSTOOD, AX_FAILED };

/* EL0-facing operations for SYS_ACTION */
enum action_op {
    ACTION_OP_SET_SETTING = 1,  /* S1: x1 = setting_id (ASET_*), x2 = value, x3 = arg_kind (AA_ABS/AA_REL) */
    ACTION_OP_FILE_EDIT   = 2,  /* S2: x1 = file_handle, x2 = offset, x3 = length, x4 = flags, x5 = user buffer */
    ACTION_OP_FILE_DELETE = 3,  /* S3: x1 = file_handle */
};

/* Action syscall result codes */
enum action_result {
    ACTION_OK              = 0,   /* Action executed successfully (S0) or review opened (S1/S2/S3) */
    ACTION_PENDING         = 1,   /* Review opened, ticket returned in x1 */
    ACTION_E_PERM          = -1,  /* No capability for this action */
    ACTION_E_REVIEW_UNAVAILABLE = -2,  /* Review path not available or ledger unhealthy */
    ACTION_E_NO_UNDO       = -3,  /* S3 action has no undo */
    ACTION_E_ARG           = -4,  /* Invalid arguments */
    ACTION_E_STATE         = -5,  /* Invalid state (e.g., no live review) */
};

struct aura_outcome {
    uint8_t state;                 /* AX_*: what really happened */
    uint8_t decision, reason;      /* the policy's answer (BP_*, BPR_*) */
    uint8_t executed;              /* 1 only if the shell state was changed or already as requested, and VERIFIED */
    struct aura_intent intent;     /* what was parsed (the preview is from constants) */
    char msg[64];                  /* the one line to show: built from constants and the validated preview, never from the typed text */
};

/* Run the S0 effect of a validated, ALLOWED intent. Returns 1 if the shell now shows what was asked, 0 otherwise (nothing is claimed). */
int aura_execute_s0(struct shell *sh, const struct aura_intent *in, const struct aura_caps *caps);

/* Turn a policy decision (and, when it was ALLOW_NOW, the execution result) into the outcome and its message. */
void aura_outcome_from(struct aura_outcome *o, const struct bp_result *d, int executed);

/* ---- S1 settings execution (Cycle 21). The chain, in this order, with every step able to stop it and say so truthfully:
 *   parse -> validate -> broker decision (REVIEW) -> resolve the previous and proposed value through the registry -> the TRUSTED REVIEW (a person approves exactly this change)
 *   -> [after approval] health check -> broker decision AGAIN with a fresh nonce -> re-read the previous value -> rv_verify (the approval is bound to every byte)
 *   -> durable PRE record in the Trail (must succeed or NOTHING runs) -> registry set -> read-back -> apply to the live shell -> durable POST record -> truthful message.
 * AuroraFS has no transactions: the setting and the Trail records are separate files, so the chain is NOT atomic. Each outcome says which parts really happened:
 * a crash between PRE and the setting write leaves a PRE without a POST, which aura_s1_recover reports at the next boot; a setting that was saved but whose POST
 * could not be written is reported as such and offers no Undo. Only the trusted shell (requester pid 0) can start this; an EL0 requester never opens a review. */
struct aura_s1_env {
    int (*ledger_ok)(void);                      /* Passport AND Trail healthy, verified and writable */
    int (*trail_append)(const struct trail_rec *r);          /* 0 only when the record is durably on the disk */
    int (*trail_newest)(struct trail_rec *out);              /* 1 and fills *out if the Trail holds at least one record */
    uint32_t (*trail_last_seq)(void);                        /* sequence number of the newest record */
    uint64_t (*now_ms)(void);                                /* monotonic milliseconds */
    int (*trail_nth)(uint32_t n, struct trail_rec *out);     /* the n-th newest record (0 = newest); 1 and fills *out, or 0 if there is none (used to find the receipt to undo) */
};
/* Undo: the typed word "undo" (trusted shell only) finds the newest S1 receipt = a PRE record immediately followed by its DONE POST record for the same setting, and only if the setting
 * STILL holds the receipt's recorded result builds the reversal "set <setting> to <previous value>", which then travels the whole S1 chain (broker, NEW trusted review, PRE/POST records,
 * read-back) like any other change. A newer change by anyone, a failed or unrecorded change, or an empty Trail means "Nothing to undo" and nothing is reviewed or written. */
int  aura_s1_undo_text(struct shell *sh, char *out, size_t cap, char *why, size_t whycap);   /* 1: *out is the reversal command; 0: *why says why not */
void aura_s1_attach(const struct aura_s1_env *e);            /* NULL: S1 is unavailable and no review is ever opened */
/* Called by aura_run_text when the broker answered REVIEW. Opens the trusted review for the shell's own request and sets the outcome (waiting / not available). */
void aura_s1_begin(struct shell *sh, const struct bp_env *env, uint32_t pid, uint64_t nonce, struct aura_outcome *out);
/* Called by the kernel loop after shell_review_take_approved. Runs the rest of the chain and reports the result on the Aura screen. Returns the AX_* state. */
int  aura_s1_complete(struct shell *sh, const struct bp_env *env, struct bp_replay *rp, uint64_t ticket, uint64_t fresh_nonce);
/* At boot: if the newest Trail record is a PRE (an S1 change was interrupted), record that it did not complete and what the setting is now. Returns 1 if it did. */
int  aura_s1_recover(struct shell *sh);
/* The shared resolution of "from what to what": 0 and *next, -1 invalid, -2 already so, -3 already at the end of the ladder. */
int  aura_s1_resolve(const struct aura_intent *in, uint32_t cur, int32_t *next);

/* Full pipeline for TEXT from the trusted shell (requester pid 0) or from any requester the kernel attested. */
void aura_run_text(struct shell *sh, const struct bp_env *env, struct bp_replay *rp, uint32_t pid, uint64_t gen, uint64_t nonce,
                   const char *text, size_t len, struct aura_outcome *out);
/* Pipeline for an intent that was already parsed and ALLOWED earlier (queued from an EL0 request): re-checks that the requester is still the same live
 * instance, re-validates the intent, then executes. A requester that exited or was replaced in between has its request dropped. */
void aura_run_queued(struct shell *sh, const struct bp_env *env, uint32_t pid, uint64_t gen, const struct aura_intent *in, struct aura_outcome *out);

/* S2 file execution (after trusted review approval) */
void aura_s2_begin(struct shell *sh, const struct bp_env *env, uint32_t pid, uint64_t nonce, struct aura_outcome *out);
int aura_s2_complete(struct shell *sh, const struct bp_env *env, struct bp_replay *rp, uint64_t ticket, uint64_t fresh_nonce);

/* S3 execution (after hold-to-confirm) */
void aura_s3_begin(struct shell *sh, const struct bp_env *env, uint32_t pid, uint64_t nonce, struct aura_outcome *out);
int aura_s3_complete(struct shell *sh, const struct bp_env *env, struct bp_replay *rp, struct hold *hd, uint64_t fresh_nonce, struct aura_outcome *out);
#endif
