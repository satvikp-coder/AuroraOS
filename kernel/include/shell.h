/*
 * kernel/include/shell.h - the native Aurora Glass shell, Tier 0 Clear (issue #8). Kernel-side bring-up: screen state, navigation
 * history, layout, input dispatch and drawing. It is deliberately split from the hardware: shell.c needs only the renderer, the scene
 * and the focus module, so it is host-tested; shell_run.c (kernel only) feeds it real input events and the clock.
 *
 * Responsibilities (kept apart on purpose):
 *   renderer (gfx_*)  pixels and shapes            scene (ui_scene)  visual elements and layers
 *   focus (ui_focus)  hit testing / focus / modal   shell (this file) screens, navigation, per-screen layout and behaviour
 * The UI is NOT an authorisation mechanism: a click or key press here is a UI event, never permission for a privileged action, and
 * no screen performs one. Everything shown is true: unavailable destinations are labelled unavailable, nothing is simulated.
 * Boundary for the future: when applications get their own EL0 UI services, the scene/focus API stays and the shell becomes the trusted
 * compositor-side screen manager; applications will never be handed the framebuffer, the scene array or the focus context.
 */
#ifndef AURORA_SHELL_H
#define AURORA_SHELL_H
#include <gfx.h>
#include <input.h>
#include <ui_focus.h>
#include <index_model.h>
#include <ui_scene.h>

enum shell_screen { SCR_HOME = 0, SCR_SPACES, SCR_INDEX, SCR_COUNT };

/* Stable control identifiers (never reused for another meaning). */
enum {
    SID_DOCK_BACK = 1, SID_DOCK_HOME, SID_DOCK_SPACES, SID_DOCK_INDEX,
    SID_CHANGE_SPACE = 10, SID_APP_SPACES, SID_APP_INDEX, SID_APP_PAGES, SID_APP_AURA, SID_APP_SETTINGS, SID_ABOUT,
    SID_ABOUT_CLOSE = 20,
    SID_CORNER = 40, SID_CORNER_CLOSE,
    SID_SPACE_BASE = 30,                /* SID_SPACE_BASE + n selects built-in Space n (0..3) */
    SID_RESULT_BASE = 100               /* SID_RESULT_BASE + k is the k-th Index result row (k < SHELL_RESULTS) */
};

#define SHELL_MAX_WIDGETS 48
#define SHELL_HISTORY 8
#define SHELL_SPACES 4
#define SHELL_RESULTS 8
#define SHELL_RECENT 8

struct shell_caps { uint8_t keyboard, pointer; };      /* what input hardware the kernel actually found */

struct shell {
    uint32_t width, height;
    uint8_t screen, space, about_open, corner_open, dirty;
    uint8_t hist[SHELL_HISTORY], hist_n;
    struct shell_caps caps;
    uint32_t uptime_s;
    int32_t px, py;
    uint32_t last_activated;            /* id of the last activation, SHELL_NONE before any */
    uint32_t activations;
    struct ui_widget w[SHELL_MAX_WIDGETS];
    struct ui_ctrl c[SHELL_MAX_WIDGETS];
    uint32_t n;
    struct ui_ctx fx;
    char uptime_txt[16];
    char space_txt[32];
    char horizon_txt[40];               /* the Aurora Horizon status text; its only source is the boot result and the input devices found */
    /* Index (search) state: bounded, in memory only */
    char query[IDX_MAX_QUERY + 1];
    uint8_t qlen;
    struct idx_result res[SHELL_RESULTS];
    uint8_t nres;
    int8_t sel;                         /* highlighted result, -1 = none */
    const char *recent[SHELL_RECENT];   /* keys of opened items, most recent first */
    uint8_t nrecent;
    char field_txt[IDX_MAX_QUERY + 8];
    char res_txt[SHELL_RESULTS][56];
    uint32_t index_opens;               /* how many results were opened (tests) */
};

#define SHELL_NONE 0xffffffffu

extern const char *const SHELL_SPACE_NAMES[SHELL_SPACES];

void shell_init(struct shell *sh, uint32_t width, uint32_t height);
void shell_set_caps(struct shell *sh, struct shell_caps caps);
void shell_set_uptime(struct shell *sh, uint32_t seconds);                   /* marks dirty only when the shown text changes */
void shell_set_pointer(struct shell *sh, int32_t x, int32_t y);
/* Route one normalised input event. Returns 1 if anything visible may have changed. */
int  shell_handle(struct shell *sh, const struct input_event *e);
/* Navigation. All return 1 if the screen changed. Unknown screens are refused. */
int  shell_go(struct shell *sh, uint32_t screen);
int  shell_back(struct shell *sh);
int  shell_select_space(struct shell *sh, uint32_t index);                  /* refuses index >= SHELL_SPACES */
/* Draw the whole frame (clear + scene + focus ring + pointer). */
int  shell_draw(struct shell *sh, const struct gfx_surface *s);
/* Index destinations (a fixed registry of REAL destinations; unavailable ones are listed but never open anything). */
size_t shell_index_count(void);
const struct idx_item *shell_index_item(size_t i);
int  shell_index_available(size_t i);
const char *shell_screen_name(uint32_t screen);
int32_t shell_find(const struct shell *sh, uint32_t id);                     /* index of the widget with this id, or -1 */

#endif
