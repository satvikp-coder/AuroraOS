/*
 * kernel/ui/shell_internal.h - internal shell types and helpers shared with review_modal.c.
 * This is NOT a public API; it exists only so the review modal can use the shell's
 * layout helpers without duplicating them.
 */
#ifndef AURORA_SHELL_INTERNAL_H
#define AURORA_SHELL_INTERNAL_H
#include <shell.h>
#include <ui_scene.h>
#include <ui_focus.h>
#include <gfx.h>

/* Font selection ladder. */
struct fnt { uint8_t size, mag; };

/* Palette definitions. */
struct pal { uint32_t canvas, surface, ink, muted, accent, active_ink, focus, border, border_dis, scrim, aura, hover, pressed, secure, secure_ink, ctl; uint8_t bw, marker, rail; };
extern const struct pal PAL_STD;
extern const struct pal PAL_HC;

/* Palette selector. */
#define PAL(sh) ((sh)->contrast ? &PAL_HC : &PAL_STD)

/* Constants used by the shell. */
#define FOC (UI_VISIBLE | UI_ENABLED | UI_FOCUSABLE)
#define SOLID UI_VISIBLE
#define COL_HOVER 0x00E4ECF6u
#define COL_PRESSED 0x00CFDCF0u
#define COL_AURA 0x0000838Cu
#define COL_DISABLED_FILL 0x00F3F6FAu

/* String helpers. */
static inline void scpy(char *d, uint32_t cap, const char *s)
{
    uint32_t i = 0;
    for (; i + 1 < cap && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
static inline void pcat(char *d, uint32_t cap, const char *s)
{
    uint32_t n = 0; while (n + 1 < cap && d[n]) n++;
    for (; n + 1 < cap && s && *s; s++) d[n++] = *s;
    d[n] = 0;
}
static inline void pnum(char *d, uint32_t cap, uint64_t v)
{
    char t[24]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 24);
    char o[25]; int m = 0;
    while (k) o[m++] = t[--k];
    o[m] = 0;
    pcat(d, cap, o);
}

/* Font/layout helpers (implemented in shell.c). */
struct fnt pick_font(uint32_t base, uint32_t pct);
int32_t lines_for(struct fnt f, int bold, const char *t, int32_t inner_w);
int32_t lh_of(struct fnt f, int bold);
int32_t adv_of(struct fnt f, int bold);

/* Widget creation helpers (implemented in shell.c). */
struct ui_widget *add(struct shell *sh, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint8_t layer, uint8_t kind,
                      const char *text, uint32_t fill, uint32_t id, uint8_t flags);
int32_t sb(struct shell *sh, int32_t x, int32_t y, int32_t w, int32_t minh, const char *t, uint32_t id, int enabled, uint8_t layer);

/* Full shell rebuild (implemented in shell.c). */
void build(struct shell *sh);
int32_t shell_find(const struct shell *sh, uint32_t id);

#endif