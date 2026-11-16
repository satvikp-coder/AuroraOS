/*
 * kernel/include/aura_exec.h - the S0 Aura executor (issue #10): the narrow, verified bridge from an authorised intent to a real shell navigation.
 *
 *   text -> aura_parse() -> aura_validate() -> bp_decide() (requester authorisation) -> ALLOW_NOW -> aura_execute_s0() -> shell_go()/shell_select_space() -> verified
 *
 * aura_execute_s0 is the ONLY code that turns an intent into an effect, and it can do exactly two things: show one of the five built-in screens, or select
 * one of the four built-in Spaces (and show Home). It has no shell-command, syscall, path or process interface. An intent is re-validated and its kind and
 * target are checked again here (defence in depth), and success is reported ONLY if the shell's state afterwards matches the request.
 * S1 and above never reach it: the policy answers REVIEW, HOLD or UNAVAILABLE and the outcome says nothing was changed.
 */
#ifndef AURORA_AURA_EXEC_H
#define AURORA_AURA_EXEC_H
#include <broker_policy.h>
#include <shell.h>

enum { AX_NONE = 0, AX_DONE, AX_DENIED, AX_UNAVAILABLE, AX_NEEDS_APPROVAL, AX_NOT_UNDERSTOOD, AX_FAILED };

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

/* Full pipeline for TEXT from the trusted shell (requester pid 0) or from any requester the kernel attested. */
void aura_run_text(struct shell *sh, const struct bp_env *env, struct bp_replay *rp, uint32_t pid, uint64_t gen, uint64_t nonce,
                   const char *text, size_t len, struct aura_outcome *out);
/* Pipeline for an intent that was already parsed and ALLOWED earlier (queued from an EL0 request): re-checks that the requester is still the same live
 * instance, re-validates the intent, then executes. A requester that exited or was replaced in between has its request dropped. */
void aura_run_queued(struct shell *sh, const struct bp_env *env, uint32_t pid, uint64_t gen, const struct aura_intent *in, struct aura_outcome *out);
#endif
