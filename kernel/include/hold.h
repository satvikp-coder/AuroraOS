/*
 * kernel/include/hold.h - the hold-to-confirm STATE MACHINE (Plan C5, spec section 2d): a deliberate, tactile confirmation that cannot be produced by a click, a
 * key repeat or a stuck key. PURE: no clock of its own (the caller passes monotonic milliseconds), no allocation, no I/O. It is NOT connected to any action and
 * does NOT enable anything: `env.hold_input` stays 0 and S3 operations (delete, send, install, share) remain unavailable; this module is only one of the
 * prerequisites for them (the others being the trusted review, Passport, Trail and a service-specific enforcement point).
 *
 * Rules (each is a test):
 *  - A hold starts on a NON-REPEAT key-down (Enter or Space on the focused control) or a pointer press inside the control. A second press while holding is ignored
 *    (it neither restarts nor extends the hold).
 *  - It confirms ONLY when time is polled and at least HOLD_MS (1500 ms) have passed since the start with the hold intact: 1499 ms never confirms, 1500 ms does.
 *    Confirmation happens in hold_poll() and nowhere else, so any release/cancel that was PROCESSED before the poll wins (fail closed).
 *  - It is cancelled by: release of the starting key/button, the pointer leaving the control (re-entering does NOT resume), any other key, Escape, a focus
 *    change and a modal change. Key-repeat events are ignored (they neither cancel nor extend).
 *  - One hold, at most one confirmation: after it confirms, nothing (a later release, repeats, polls) can confirm again; a NEW hold needs a new press after the
 *    release.
 *  - Time that goes backwards counts as no time having passed.
 *  - Progress for display is floor(elapsed / 375) of 4 segments, 0 unless holding or just confirmed.
 */
#ifndef AURORA_HOLD_H
#define AURORA_HOLD_H
#include <stdint.h>

#define HOLD_MS 1500u
#define HOLD_SEGMENTS 4u
#define HOLD_SEGMENT_MS 375u

enum hold_state { HD_IDLE = 0, HD_HOLDING, HD_CONFIRMED, HD_CANCELLED };
enum hold_source { HS_NONE = 0, HS_KEY, HS_POINTER };
enum hold_event {
    HE_KEY_DOWN = 1,        /* Enter/Space went down on the focused control (repeat flag says whether this is auto-repeat) */
    HE_KEY_UP,              /* the starting key went up */
    HE_OTHER_KEY,           /* any other key went down (Tab included) */
    HE_ESCAPE,
    HE_PTR_DOWN,            /* the pointer button went down INSIDE the control */
    HE_PTR_UP,
    HE_PTR_LEAVE,           /* the pointer moved outside the control */
    HE_PTR_ENTER,           /* the pointer moved back inside: never resumes a hold */
    HE_FOCUS_CHANGE,
    HE_MODAL_CHANGE
};

struct hold {
    uint8_t state, source;
    uint64_t t_start, last_now;
    uint32_t confirmations;     /* ever, for tests; a single hold can add at most 1 */
    uint32_t anomalies;
};

void hold_init(struct hold *h);
/* Feed an input event at the given monotonic time. repeat != 0 marks a key auto-repeat (ignored). Never confirms. */
void hold_event(struct hold *h, int ev, uint64_t now_ms, int repeat);
/* Poll the clock. Returns 1 exactly once per hold: when it is HOLDING and >= HOLD_MS have passed since the start. */
int  hold_poll(struct hold *h, uint64_t now_ms);
/* How many of the 4 segments are filled (0..4): floor(elapsed / 375) while holding, 4 after a confirmation, else 0. */
uint32_t hold_segments(const struct hold *h, uint64_t now_ms);
const char *hold_state_name(int state);
#endif
