/*
 * kernel/ui/review_modal.h - the trusted-review compositor modal UI.
 * Draws the secure review dialog on the SCRIM/MODAL layers, manages its focus,
 * and reports the person's answer back to the review state machine.
 * PURE: no allocation, no clock, no libc; all drawing goes through the shell's
 * widget array and the compositor's scene/focus modules.
 */
#ifndef AURORA_REVIEW_MODAL_H
#define AURORA_REVIEW_MODAL_H
#include <stdint.h>
#include <review.h>
#include <shell.h>       /* for struct shell, struct geo, UI_LAYER_* */

/* Build the review modal's widgets into the shell's widget array.
 * The modal sits on SCRIM (full-screen dim) and MODAL (the dialog) layers,
 * above everything the shell and every client can draw.
 * The shell must have sh->review_open = 1 and sh->rv populated before calling. */
void review_modal_build(struct shell *sh, const struct geo *g);

/* Close the review modal: pop the focus modal layer, clear review_open,
 * and rebuild the shell (so the underlying screen is drawn again).
 * If the review was live (OPEN/ARMED), its ticket is already retired by the
 * caller (review_decline or timeout); if it was already terminal (EXPIRED/GONE),
 * this just dismisses the dialog. */
void review_modal_close(struct shell *sh);

/* Decline an open review: if the state machine is still live, retire the
 * ticket (DECLINED), then close the modal. Escape and the Cancel button
 * both route here. */
void review_modal_decline(struct shell *sh);

/* Update the review modal's live status text (arming state, timeout, etc.)
 * and rebuild. Called from shell_review_attention and review_tick when
 * the state machine transitions. */
void review_modal_refresh(struct shell *sh);

#endif