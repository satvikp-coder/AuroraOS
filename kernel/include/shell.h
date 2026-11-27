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
#include <ui_proto.h>
#include <procsnap.h>
#include <aura_cmd.h>
#include <review.h>
#include <mic_indicator.h>
#include <hold.h>

enum shell_screen { SCR_HOME = 0, SCR_SPACES, SCR_INDEX, SCR_SETTINGS, SCR_PAGES, SCR_AURA, SCR_CLOCK, SCR_TRAIL, SCR_HOLD, SCR_COUNT };

/* Stable control identifiers (never reused for another meaning). */
enum {
    SID_DOCK_BACK = 1, SID_DOCK_HOME, SID_DOCK_SPACES, SID_DOCK_INDEX,
    SID_CHANGE_SPACE = 10, SID_APP_SPACES, SID_APP_INDEX, SID_APP_PAGES, SID_APP_AURA, SID_APP_SETTINGS, SID_ABOUT,
    SID_ABOUT_CLOSE = 20,
    SID_CORNER = 40, SID_CORNER_CLOSE,
    SID_SPACE_BASE = 30,                /* SID_SPACE_BASE + n selects built-in Space n (0..3) */
    SID_RESULT_BASE = 100,              /* SID_RESULT_BASE + k is the k-th Index result row (k < SHELL_RESULTS) */
    SID_SET_VALUE = 200,                /* SID_SET_VALUE + r changes the value of Settings row r (r < SHELL_SET_ROWS) */
    SID_SET_RESET = 210,                /* SID_SET_RESET + r restores its default */
    SID_SET_UP = 220, SID_SET_DOWN,     /* scroll the Settings list when it does not fit */
    SID_PAGES_HOME = 230,               /* Pages: return to Home */
    SID_AURA_RUN = 240, SID_AURA_CLEAR, SID_AURA_HOME,  /* Aura: run the typed command, clear it, return to Home */
    SID_CLOCK_P1 = 250, SID_CLOCK_P5, SID_CLOCK_P25, SID_CLOCK_START, SID_CLOCK_CANCEL, SID_CLOCK_HOME,  /* Clock: presets 1/5/25 min, Start, Cancel, Home */
    SID_HOLD_BTN = 270, SID_HOLD_HOME,  /* Hold-to-confirm DEMO: the hold button and Home. It confirms nothing real and enables no action (env.hold_input stays 0) */
    SID_TRAIL_NEWER = 260, SID_TRAIL_OLDER, SID_TRAIL_HOME,  /* Trail: show newer / older records, Home. There is no other control: the screen is read-only */
    SID_REVIEW_CANCEL = 21, SID_REVIEW_APPROVE = 22          /* the trusted review: Cancel (or Close once it ended) and Approve */
};

#define SHELL_MAX_WIDGETS 160
#define SHELL_HISTORY 8
#define SHELL_SPACES 4
#define SHELL_RESULTS 8
#define SHELL_RECENT 8
#define SHELL_SET_ROWS 7                /* the six registry settings plus the session-only high-contrast switch */

/* The timer service the Clock screen drives (the kernel wires it to timersys as the shell's OWN owner identity). NULL = no service: the Clock says so and
 * starts nothing. Every call returns the service's result: start returns an id >= 0 or a negative error; query returns 0 and fills state (1 running,
 * 2 done, 3 cancelled) and the remaining milliseconds; cancel and release return 0 or a negative error. */
struct shell_timers {
    int (*start)(uint64_t duration_ms);
    int (*query)(int id, uint8_t *state, uint64_t *remaining_ms);
    int (*cancel)(int id);
    int (*release)(int id);
};
enum { CK_READY = 0, CK_RUNNING, CK_DONE, CK_CANCELLED, CK_UNAVAILABLE, CK_ERROR };
#define SHELL_TOAST_MS 4000u

/* What the Trail screen shows: a COPY made by the kernel from the ledgers (no pointer into them, nothing the screen could use to change them). */
enum { TV_UNAVAILABLE = 0, TV_RECORDING, TV_NODISK, TV_READONLY, TV_UNREADABLE, TV_ROLLBACK, TV_HELD };
struct shell_trail_rec { uint32_t seq, principal; int32_t arg; uint8_t kind, target, risk, decision, reason, state; };
struct shell_trail_grant { uint32_t principal; uint8_t perm; };
#define SHELL_TV_RECS 48
#define SHELL_TV_GRANTS 32
struct shell_trail_view {
    uint8_t health;                                   /* TV_* */
    uint32_t n, ngrants;                              /* records (oldest first, <= SHELL_TV_RECS), durable grants (<= SHELL_TV_GRANTS) */
    struct shell_trail_rec rec[SHELL_TV_RECS];
    struct shell_trail_grant grant[SHELL_TV_GRANTS];
};

/* The kernel's application launcher as the shell sees it. The shell never names an executable or an identity: it passes a stable application id and the kernel decides. */
struct shell_launcher { int (*available)(uint32_t app); int (*launch)(uint32_t app); const char *(*describe)(int rc); };
struct shell_caps { uint8_t keyboard, pointer; };      /* what input hardware the kernel actually found */

/* The settings store the shell talks to (the kernel wires it to the typed registry, kernel/settings.c, as the shell's OWN principal). It returns the
 * registry's SETTINGS_* codes: 0 = saved, negative = refused or not saved (nothing changed). With no store the shell keeps session-only values and
 * says so. The shell never grants itself a capability: a refusal (SETTINGS_EPERM) is shown as a refusal. */
struct shell_store {
    int (*get)(uint32_t id, uint32_t *value);
    int (*set)(uint32_t id, uint32_t value);
    int (*reset)(uint32_t id);
};

/* What the last Settings action did (shown on screen and logged). */
enum { SM_NONE = 0, SM_SAVED, SM_RESET_SAVED, SM_SESSION, SM_STORE_FAIL, SM_DENIED, SM_VERSION, SM_HC_ON, SM_HC_OFF, SM_AURA_LOCAL };

struct shell {
    uint32_t width, height;
    uint8_t screen, space, about_open, corner_open, review_open, dirty;
    uint8_t contrast;                   /* 1 = high-contrast appearance (a runtime preference; nothing persists it yet) */
    uint8_t text_pct;                   /* text scale in percent, SHELL_TEXT_MIN..SHELL_TEXT_MAX (100 = the reference layout) */
    int32_t lay_bottom;                 /* lowest content pixel of the last scaled layout (layout fitting and tests) */
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
    const struct shell_launcher *launcher;   /* NULL = no application can be launched from Index */
    char idx_note[64];                       /* the honest result of the last failed launch, shown in place of the Index hint */
    /* EL0 client surfaces (read-only view owned by the kernel; NULL = none). The compositor draws them in the content band, below the dock, the corner,
     * sheets and modals; chrome text (the application name) comes from client_name, never from the client. */
    const struct uip_state *clients;
    const char *(*client_name)(uint8_t pid);
    /* Pages screen: a read-only snapshot of the process table, refreshed by the run loop (shell_pages_refresh); NULL source = unavailable */
    uint32_t (*pages_src)(struct procsnap *out);
    struct procsnap ps; uint64_t ps_digest; uint8_t ps_valid;
    char pg_txt[PROC_MAX][96]; char pg_head[80]; char pg_sum[40]; char pg_more[48];
    /* Aura screen: a deterministic quick-command assistant (no language model). Typed text is bounded; the live preview comes from aura_parse (constants only);
     * running is delegated to aura_run, which the kernel wires to the Action Broker as the shell itself. NULL = not connected: nothing can run. */
    int (*aura_run)(struct shell *sh, const char *text, size_t len, uint8_t *state, char *msg, size_t cap);
    char aura_in[AURA_CMD_MAX + 1]; uint8_t aura_len;
    struct aura_intent aura_pv;
    char aura_msg[64]; uint8_t aura_state; uint32_t aura_runs;       /* aura_state: the AX_* outcome of the last run (0 = none yet) */
    char aura_field[AURA_CMD_MAX + 40], aura_prev[96], aura_res[96];
    /* Clock screen and the completion toast. Time comes ONLY from the monotonic counter via shell_tick; the shell never invents a date. */
    const struct shell_timers *timers;  /* NULL = unavailable */
    uint8_t clk_state, clk_preset, clk_err;   /* CK_*, 0..2 (1, 5, 25 min), error kind (1 limit, 2 refused, 3 lost) */
    int32_t clk_id; uint32_t clk_rem_s, clk_dur_s; uint32_t clk_seq;   /* the service's timer id, whole seconds left (rounded up), count of spoken-line changes */
    uint64_t now_ms, toast_until_ms; uint8_t toast_on;
    struct mic_indicator mic;
    char clk_live[64], clk_val[12], clk_up[16];
    /* Hold-to-confirm demo (compositor-owned: no client can start, feed or confirm it). The pure state machine decides; the screen only shows it. */
    struct hold hd; uint16_t hd_key; uint8_t hd_msg, hd_seg; uint32_t hd_seq; char hd_live[40];
    /* Trail screen: read-only view of the audit ledger; NULL source = not available */
    int (*trail_src)(struct shell_trail_view *out);
    struct shell_trail_view tv; uint64_t tv_digest; uint8_t tv_valid;
    uint8_t tr_first;                                 /* index (from the newest = 0) of the first record shown */
    uint32_t tr_seq; char tr_live[80];                /* the health line, spoken (logged) when it changes */
    char tr_rows[10][88], tr_head[80], tr_sum[48], tr_perm[40], tr_range[40], tr_gr[8][56];
    /* Trusted review (compositor-owned modal; the state machine is review.c). Only kernel code opens it; no client message can open, arm, answer or close it. */
    struct rv rv; struct rv_request rv_req;
    uint8_t rv_prev, rv_lapsed, rv_approved_pending; uint64_t rv_approved_ticket; uint32_t rv_seq;
    char rv_name[24], rv_l_req[48], rv_l_chg[64], rv_l_live[72];
    /* Settings screen */
    const struct shell_store *store;    /* NULL = no usable store: changes last for this session only */
    uint32_t sess[SHELL_SET_ROWS];      /* session values (used when there is no store, and for high contrast always) */
    uint8_t set_first;                  /* first visible Settings row (the list scrolls when it does not fit) */
    uint8_t set_msg, set_row;           /* SM_* of the last action and the row it concerned */
    uint32_t set_seq;                   /* number of Settings actions so far (tests, log) */
    char set_val[SHELL_SET_ROWS][24];   /* displayed value text per row */
    char set_lab[SHELL_SET_ROWS][56];   /* displayed row label per row */
};

#define SHELL_NONE 0xffffffffu
#define SHELL_TEXT_MIN 75
#define SHELL_TEXT_MAX 200

/* Text scale: the percentage maps every text role to the nearest size of a fixed ladder (12 14 16 18 24 px native, and 24 28 32 36 48 px
 * by exact 2x magnification of the same 1-bit glyphs), so 100 and 200 percent are exact and values between snap to a real size. Text
 * wraps instead of being cut; at the largest scales secondary lines are omitted rather than clipped. Out-of-range values are refused. */
int  shell_set_text_scale(struct shell *sh, uint32_t pct);
int  shell_set_contrast(struct shell *sh, int on);
/* Attach the process-snapshot source for the Pages screen and take a first snapshot. */
void shell_set_pages(struct shell *sh, uint32_t (*src)(struct procsnap *out));
/* Re-take the snapshot; returns 1 (and redraws when Pages is showing) only if something displayed changed. */
int  shell_pages_refresh(struct shell *sh);
/* Attach the Aura runner. It must return 1 and fill state/msg when it ran the pipeline, 0 if it could not. */
void shell_set_aura(struct shell *sh, int (*run)(struct shell *sh, const char *text, size_t len, uint8_t *state, char *msg, size_t cap));
/* Trusted review. shell_review_open shows the modal for a validated S1 settings request and returns its ticket (> 0) or a negative RV_E_*: RV_E_STATE when there is no
 * keyboard (a review needs one), RV_E_BUSY while another modal or review is open. requester_name comes from the KERNEL's process table, never from the client.
 * shell_review_attention is the secure-attention gesture (kernel input path only). shell_review_take_approved returns 1 once, with the ticket, after the person approved;
 * the executor must then call shell_review_verify (which re-checks everything and consumes the approval). Nothing here executes any change. */
int64_t shell_review_open(struct shell *sh, uint64_t now_ms, const struct rv_request *q, const struct aura_caps *caps, const char *requester_name);
int  shell_review_attention(struct shell *sh, uint64_t now_ms, int repeat);
int  shell_review_requester_gone(struct shell *sh, uint32_t pid, uint64_t gen);
int  shell_review_take_approved(struct shell *sh, uint64_t *ticket);
int  shell_review_verify(struct shell *sh, uint64_t now_ms, uint64_t ticket, const struct rv_request *now_q, const struct aura_caps *caps);
void shell_set_now_ms(struct shell *sh, uint64_t now_ms);
/* Show the result of an Aura action that finished AFTER the command was typed (the S1 chain): the Aura screen's result line and state. */
void shell_aura_report(struct shell *sh, uint8_t state, const char *msg);             /* the monotonic time of the input about to be handled (kernel loop) */

/* Attach the ledger view for the Trail screen and take a first copy; shell_trail_refresh re-takes it (returns 1 and redraws only if something shown changed). */
void shell_set_trail(struct shell *sh, int (*src)(struct shell_trail_view *out));
int  shell_trail_refresh(struct shell *sh);
/* Attach (or detach, with NULL) the timer service for the Clock screen. */
void shell_set_timers(struct shell *sh, const struct shell_timers *timers);
/* Aura's timer effects (called by the S0 executor after the broker allowed them). Each returns 1 ONLY if the real timer service now shows what was asked:
 * start reads the timer back from the service (running, no more than the requested time left); cancel needs a running timer and the service's confirmation.
 * A timer that is already running is never replaced. Nothing is claimed otherwise. */
int  shell_clock_start(struct shell *sh, uint32_t seconds);
int  shell_clock_cancel(struct shell *sh);
/* Advance the shell's view of the monotonic clock (milliseconds since boot). Polls a running timer, completes it (once), expires the toast. Independent of
 * the visible screen. Returns 1 if anything visible changed. */
int  shell_tick(struct shell *sh, uint64_t now_ms);
void shell_set_clients(struct shell *sh, const struct uip_state *st, const char *(*name_of)(uint8_t pid));
/* Attach (or detach, with NULL) the settings store; the stored text scale is applied at once. */
void shell_set_store(struct shell *sh, const struct shell_store *store);
/* Settings table (read-only facts for tests): row count, registry id (0 = the session-only high-contrast row), number of values. */
uint32_t shell_setting_id(uint32_t row);
uint32_t shell_setting_values(uint32_t row, uint32_t *vals, uint32_t max);
uint32_t shell_setting_default(uint32_t row);
/* The text size the shell uses for a role whose reference size is `base` px at 100 percent (exposed for tests). */
void shell_font_for(uint32_t base, uint32_t pct, uint8_t *size, uint8_t *mag);

extern const char *const SHELL_SPACE_NAMES[SHELL_SPACES];

void shell_init(struct shell *sh, uint32_t width, uint32_t height);
void shell_set_caps(struct shell *sh, struct shell_caps caps);
void shell_set_launcher(struct shell *sh, const struct shell_launcher *l);   /* NULL: application entries in Index are listed as not available */
void shell_set_uptime(struct shell *sh, uint32_t seconds);                   /* marks dirty only when the shown text changes */
int  shell_mic_begin(struct shell *sh, uint32_t principal, uint32_t pid, uint64_t gen);
int  shell_mic_stop(struct shell *sh);
void shell_mic_owner_gone(struct shell *sh, uint32_t pid, uint64_t gen);
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
