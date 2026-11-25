/*
 * kernel/include/aura_cmd.h - Aura Quick Commands: a DETERMINISTIC parser from a short ASCII command to a typed, bounded intent (issue #10 foundation).
 *
 * What this is: a pure function (no state, no clock, no allocation, no libc) that turns text into one `struct aura_intent` made only of enumerations
 * and small validated numbers. The preview text is built from constants and validated numbers: it never echoes user text. Unsupported things are
 * reported as unsupported; nothing is ever reported as done.
 *
 * What this is NOT: an authorisation service, an executor or a model. A parsed intent authorises NOTHING. It is data that a future native Action
 * Broker will check against the caller's kernel-assigned identity, the risk class and trusted approval before anything happens. The future Qwen3-0.6B
 * model will propose THE SAME intent structure; it will never execute anything, and its output is untrusted exactly like typed input, so
 * aura_validate() re-checks every intent independently of the parser.
 *
 * Risk classes follow DESIGN.md: S0 simple view/open (one safe step may run immediately), S1 reversible settings (plan preview), S2 file modifications
 * (explicit review), S3 delete/send/install/share (tactile hold confirmation, never voice-only). A status of AS_ACTIONABLE means only "this is an S0
 * navigation that policy allows to run without a review"; it is still the broker's decision to run it.
 *
 * Grammar: the WHOLE input must match one command. Anything left over is UNKNOWN; there are no partial executions. One command at a time: "and",
 * "then", ";" make the input a CLARIFY. Text that tries to instruct the assistant ("ignore previous instructions ...") is REFUSED as a whole.
 */
#ifndef AURORA_AURA_CMD_H
#define AURORA_AURA_CMD_H
#include <stdint.h>
#include <stddef.h>

#define AURA_CMD_MAX 64               /* input bytes; longer input is rejected, never truncated */
#define AURA_INTENT_VERSION 1
#define AURA_PREVIEW_MAX 48

enum aura_kind {
    AK_NONE = 0,
    AK_OPEN_SCREEN,
    AK_SELECT_SPACE,
    AK_SET_SETTING,
    AK_OPEN_APP,          /* open a native EL0 application (calculator, notes, files) */
    /* S2: file modifications (explicit review, risk 2) */
    AK_EDIT_FILE,
    AK_RENAME_FILE,
    AK_MOVE_FILE,
    /* S3: delete/send/install/share (tactile hold confirmation, risk 3) */
    AK_DELETE_FILE,
    AK_SEND_FILE,
    AK_INSTALL_APP,
    AK_SHARE_FILE,
    AK_UNSUPPORTED,
    AK_DESTRUCTIVE,
    AK_CLARIFY,
    AK_UNKNOWN,
    AK_REFUSED,
    AK_TIMER,   /* additive in schema v1 (a consumer that does not know it rejects the kind): start (target 0, arg = seconds 1..86400) or cancel (target 1) the shell's timer */
    AK_COUNT
};
enum aura_status { AS_ACTIONABLE = 0,      /* an S0 navigation the policy lets run without review (still the broker's decision) */
                   AS_NEEDS_APPROVAL,      /* S1 or higher: needs a preview and the documented approval before any effect */
                   AS_UNSUPPORTED,         /* understood, but no native service exists: it will not be done */
                   AS_CLARIFY,             /* ambiguous, incomplete or several commands: nothing is done */
                   AS_REJECTED,            /* invalid input or out-of-range value, or a request the native OS cannot honour yet */
                   AS_COUNT };
enum aura_reason { AR_NONE = 0, AR_EMPTY, AR_TOO_LONG, AR_BAD_ENCODING, AR_INJECTION, AR_MULTIPLE, AR_AMBIGUOUS, AR_MISSING_ARG, AR_RANGE, AR_NOT_UNDERSTOOD,
                   AR_NO_TIMER, AR_NO_HARDWARE, AR_NO_NOTES, AR_NO_MESSAGING, AR_NO_PAGES, AR_NEEDS_BROKER,
                   /* S2/S3 specific reasons */
                   AR_NO_FILES, AR_NO_PERMISSION, AR_NO_TARGET, AR_NO_DESTINATION, AR_NO_APP, AR_NO_CONTACT,
                   AR_COUNT };
enum aura_screen { ASC_HOME = 0, ASC_SPACES, ASC_INDEX, ASC_SETTINGS, ASC_PAGES, ASC_CLOCK, ASC_COUNT };      /* AK_OPEN_SCREEN targets */
enum aura_arg { AA_NONE = 0, AA_ABS, AA_REL };

/* AK_TIMER targets and limits. A duration must carry an explicit unit (seconds, minutes or hours): a bare number is never guessed. The bare
 * "timer 25" and a backslash form are NOT commands: they ask "How long? Say minutes or seconds.". */
#define ATM_START 0
#define ATM_CANCEL 1
#define ATM_MIN_S 1
#define ATM_MAX_S 86400

/* registry setting ids (docs/kernel/settings.md): 1 theme, 2 text scale, 3 reduced motion, 4 focus mode, 5 aura mode, 6 notifications, 7 space */
#define ASET_THEME 1
#define ASET_TEXT_SCALE 2
#define ASET_REDUCED_MOTION 3
#define ASET_FOCUS_MODE 4
#define ASET_AURA_MODE 5
#define ASET_NOTIFICATIONS 6
#define ASET_SPACE 7

struct aura_intent {
    uint8_t version;          /* AURA_INTENT_VERSION */
    uint8_t kind;             /* enum aura_kind */
    uint8_t status;           /* enum aura_status */
    uint8_t reason;           /* enum aura_reason: why it is not actionable (AR_NONE when it is) */
    uint8_t risk;             /* 0..3 = S0..S3: the risk class the action WOULD have */
    uint8_t target;           /* screen (enum aura_screen) | Space index 0..3 | setting id */
    uint8_t arg_kind;         /* enum aura_arg: for AK_SET_SETTING */
    int32_t arg;              /* AA_ABS: the value (theme 0 auto, 1 light, 2 dark; text scale percent 75..200); AA_REL: -1 smaller, +1 larger */
    char preview[AURA_PREVIEW_MAX];   /* fixed text for the review screen, built from constants only */
};

/* What the native OS actually provides. A zero field means "does not exist": the matching command is reported unsupported. */
struct aura_caps { uint8_t pages, timer, notes, calculator, files, messaging, hardware; };

/* Parse one command. `len` bytes of `text` are read (no NUL needed). Always fills *out; returns out->status. Never reads past len. */
int aura_parse(const char *text, size_t len, const struct aura_caps *caps, struct aura_intent *out);

/* Independent validator: 1 if the intent is well formed and consistent (version, enums, target and argument ranges, risk, status/kind/reason
 * relationship, canonical preview) and 0 otherwise. Shares no parsing code with aura_parse. Use it on EVERY intent before acting on it, including
 * intents that did not come from the parser. */
int aura_validate(const struct aura_intent *in, const struct aura_caps *caps);

const char *aura_kind_name(uint8_t kind);
const char *aura_status_name(uint8_t status);
const char *aura_reason_name(uint8_t reason);
#endif
