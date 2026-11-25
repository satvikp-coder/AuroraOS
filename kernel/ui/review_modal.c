/* kernel/ui/review_modal.c - the trusted-review compositor modal UI (see review_modal.h). */
#include "review_modal.h"
#include <review.h>
#include <ui_focus.h>
#include "shell_internal.h"

/* Percentage formatter for the preview text. */
static void rv_pct(char *d, uint32_t cap, int32_t v)
{
    uint32_t n = 0;
    if (v < 0) v = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v && n + 1 < cap);
    for (uint32_t i = 0; i < n / 2; i++) { char t = d[i]; d[i] = d[n - 1 - i]; d[n - 1 - i] = t; }
    if (n + 1 < cap) d[n++] = '%';
    d[n] = 0;
}

static void rv_val(char *d, uint32_t cap, uint8_t target, int32_t v)
{
    if (target == 2) rv_pct(d, cap, v);
    else if (v == 0) scpy(d, cap, "Auto");
    else if (v == 1) scpy(d, cap, "Light");
    else if (v == 2) scpy(d, cap, "Dark");
    else scpy(d, cap, "unknown");
}

/* Build the three live text lines for the modal. */
static void rv_build_texts(struct shell *sh)
{
    sh->rv_l_req[0] = sh->rv_l_chg[0] = sh->rv_l_live[0] = 0;

    /* "Requested by: <kernel-attested name>" */
    pcat(sh->rv_l_req, sizeof sh->rv_l_req, "Requested by: ");
    pcat(sh->rv_l_req, sizeof sh->rv_l_req, sh->rv_name[0] ? sh->rv_name : "unknown");

    /* "Text size: <prior> -> <next>" or "Theme: <prior> -> <next>" */
    uint8_t tg = sh->rv_req.intent.target;
    pcat(sh->rv_l_chg, sizeof sh->rv_l_chg, tg == 2 ? "Text size: " : tg == 1 ? "Theme: " : "Setting: ");
    rv_val(sh->rv_l_chg, sizeof sh->rv_l_chg, tg, sh->rv_req.prior);
    pcat(sh->rv_l_chg, sizeof sh->rv_l_chg, " -> ");
    rv_val(sh->rv_l_chg, sizeof sh->rv_l_chg, tg, sh->rv_req.next);

    /* Live status line */
    const char *l;
    if (sh->rv.state == RV_ARMED)           l = "Approve is enabled for 10 seconds.";
    else if (sh->rv.state == RV_EXPIRED)    l = "Review expired. Nothing changed.";
    else if (sh->rv.state == RV_GONE)       l = "Requester exited. Nothing changed.";
    else if (sh->rv_lapsed)                 l = "Approve timed out. Press Ctrl+Alt+Delete again.";
    else                                    l = "Press Ctrl+Alt+Delete to enable Approve.";
    pcat(sh->rv_l_live, sizeof sh->rv_l_live, l);
}

/* Add a wrapped label on the MODAL layer. Returns its height. */
static int32_t ml(struct shell *sh, int32_t x, int32_t y, int32_t w, const char *t,
                  uint32_t base, uint32_t ink, int bold, uint32_t id)
{
    struct fnt f = pick_font(base, sh->text_pct);
    int32_t hh = lines_for(f, bold, t, w) * lh_of(f, bold) + 4;
    struct ui_widget *k = add(sh, x, y, w, hh, 0, UI_LAYER_MODAL, UI_LABEL, t,
                              PAL(sh)->surface, id, SOLID);
    if (k) { k->font_size = f.size; k->mag = f.mag; k->bold = (uint8_t)bold; k->wrap = 1; k->ink = ink; }
    return hh;
}

/* Build the review modal into the shell's widget array.
 * The modal sits on SCRIM (full-screen dim) and MODAL (the dialog) layers. */
void review_modal_build(struct shell *sh, const struct geo *g)
{
    const struct pal *P = PAL(sh);
    rv_build_texts(sh);

    add(sh, 0, 0, g->W, g->H, 0, UI_LAYER_SCRIM, UI_PANEL, 0, P->scrim, 0xF200u, SOLID);

    int32_t w = 640 * (int32_t)sh->text_pct / 100;
    if (w < 640) w = 640;
    if (w > g->W - 48) w = g->W - 48;
    int32_t x = (g->W - w) / 2, y = g->top, iw = w - 32;

    struct ui_widget *d = add(sh, x, y, w, 100, 20, UI_LAYER_MODAL, UI_PANEL, 0, P->surface, 0xF201u, SOLID);
    if (d) { d->border_w = 2; d->border = P->focus; }

    int32_t iy = y + 16;
    struct fnt f16 = pick_font(16, sh->text_pct);
    int32_t sh_h = lh_of(f16, 1) + 16;
    if (sh_h < 40) sh_h = 40;

    /* The secure strip: fixed text, the only user of the secure colour. */
    struct ui_widget *strip = add(sh, x + 16, iy, iw, sh_h, 8, UI_LAYER_MODAL, UI_PANEL, 0, P->secure, 0xF210u, SOLID);
    if (strip) { strip->border_w = 2; strip->border = P->ink; }
    struct ui_widget *st = add(sh, x + 18, iy + (sh_h - lh_of(f16, 1)) / 2, iw - 4, lh_of(f16, 1),
                               0, UI_LAYER_MODAL, UI_LABEL, "AuroraOS secure review", P->secure, 0xF211u, SOLID);
    if (st) { st->font_size = f16.size; st->mag = f16.mag; st->bold = 1; st->ink = P->secure_ink; }

    iy += sh_h + 8;

    /* Requester line */
    iy += ml(sh, x + 16, iy, iw, sh->rv_l_req, 16, P->ink, 0, 0xF212u) + 2;
    /* Canonical preview from the intent */
    iy += ml(sh, x + 16, iy, iw, sh->rv_req.intent.preview, 18, P->ink, 1, 0xF213u) + 2;
    /* Change line */
    iy += ml(sh, x + 16, iy, iw, sh->rv_l_chg, 16, P->ink, 0, 0xF214u) + 2;
    /* Risk/notes line */
    iy += ml(sh, x + 16, iy, iw,
             sh->rv_req.intent.target == 1
                 ? "Reversible. Recorded in Trail. Stored only: no dark theme."
                 : "Reversible. Recorded in Trail.",
             14, P->muted, 0, 0xF215u) + 2;
    /* Live status line */
    iy += ml(sh, x + 16, iy, iw, sh->rv_l_live, 14, P->ink, 1, 0xF216u) + 8;

    /* Buttons: Cancel/Close added BEFORE Approve so default focus is Cancel.
     * Enter or Space on the default focus therefore DECLINES. */
    int32_t gap = 8, bw = (iw - gap) / 2, armed = sh->rv.state == RV_ARMED;
    int ended = (sh->rv.state == RV_EXPIRED || sh->rv.state == RV_GONE);

    int32_t h1 = sb(sh, x + 16, iy, bw, 48, ended ? "Close" : "Cancel", SID_REVIEW_CANCEL, 1, UI_LAYER_MODAL);
    int32_t h2 = sb(sh, x + 16 + bw + gap, iy, bw, 48, armed ? "Approve" : "Approve (locked)", SID_REVIEW_APPROVE, armed, UI_LAYER_MODAL);

    iy += (h1 > h2 ? h1 : h2);
    if (d) d->c.h = iy + 16 - y;
}

/* Close the review modal: pop the focus modal layer, clear review_open,
 * and rebuild the shell (so the underlying screen is drawn again). */
void review_modal_close(struct shell *sh)
{
    if (!sh->review_open) return;
    ui_modal_pop(&sh->fx);
    uint32_t keep = sh->fx.focus >= 0 ? sh->c[sh->fx.focus].id : SHELL_NONE;
    sh->review_open = 0;
    sh->rv_seq++;
    build(sh);  /* rebuild the underlying screen */
    if (keep != SHELL_NONE) { int32_t i = shell_find(sh, keep); if (i >= 0) ui_focus_set(&sh->fx, i); }
    sh->dirty = 1;
}

/* Decline an open review: if the state machine is still live, retire the
 * ticket (DECLINED), then close the modal. Escape and the Cancel button
 * both route here. */
void review_modal_decline(struct shell *sh)
{
    if (!sh->review_open) return;
    uint8_t was_live = (sh->rv.state == RV_OPEN || sh->rv.state == RV_ARMED || sh->rv.state == RV_APPROVED);
    uint64_t ticket = sh->rv.ticket;
    uint8_t target = sh->rv_req.intent.target;
    uint32_t now_ms = (uint32_t)sh->now_ms;
    if (was_live)
        rv_decline(&sh->rv, ticket);
    /* Trail: REVIEW_DECLINE (kind 5, state 4, arg = ticket) */
    if (was_live && sh->review_trail_append) {
        (void)sh->review_trail_append(5, 4, target, 1, (uint32_t)ticket, now_ms);
    }
    review_modal_close(sh);
}

/* Refresh the review modal's live status text and rebuild.
 * Called when the state machine transitions (arming, lapse, timeout, etc.). */
void review_modal_refresh(struct shell *sh)
{
    if (!sh->review_open) return;
    sh->rv_seq++;
    build(sh);
    sh->dirty = 1;
}