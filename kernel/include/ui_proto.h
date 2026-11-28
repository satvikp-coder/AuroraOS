/*
 * kernel/include/ui_proto.h - the EL0 UI protocol, version 1 (phase P1 of docs/kernel/el0-ui-service-design.md).
 *
 * This is the PURE part: wire format, validation and the compositor's model of client surfaces. It has no scheduler, no syscall, no device and no
 * libc dependency, so it is host-tested (tests/kernel-host/ui_proto_test.c). Nothing here draws: the compositor reads the VISIBLE model
 * (uip_surface) and draws it with the scene it owns. A client describes bounded content INSIDE a surface the compositor allotted; it never names
 * pixels, layers, z-order, focus, the framebuffer or its own identity.
 *
 * Wire format (a message is at most UIP_MSG_MAX = 64 bytes, the existing IPC message size; all multi-byte fields are LITTLE ENDIAN at the
 * explicit byte offsets below, so nothing depends on struct layout or padding):
 *
 *   0      version  u8   = 1
 *   1      type     u8
 *   2..3   seq      u16  chosen by the sender, echoed in the STATUS reply
 *   4..7   handle   u32  a surface handle = (serial << 8) | slot; 0 = none. Not a pointer.
 *   8..    payload
 *
 *   CREATE_SURFACE (1)   8: size_class u8 (0 small, 1 medium, 2 large)  9: title_len u8 (<= 24)  10..: title ASCII      handle must be 0
 *   SET_TEXT (2)         8: widget u8 (< 16)  9: style u8 (< 4)  10: text_len u8 (<= 40)  11..: text ASCII
 *   SET_RECT (3)         8: widget u8  9: role u8 (< 4)  10..11: x i16  12..13: y i16  14..15: w u16  16..17: h u16
 *   COMMIT (4)           no payload     atomic: staged changes become visible all at once or not at all
 *   DESTROY_SURFACE (5)  no payload
 *   REQUEST_FOCUS (6)    no payload     ADVISORY: records a wish; only the compositor decides focus
 *   STATUS (0x81)        8..9: code i16  10: request type u8       (handle = the surface concerned; for CREATE the new handle)
 *   EVENT_KEY (0x82)     8..9: evdev code u16  10..11: modifiers u16  12: down u8  13: repeat u8
 *   EVENT_POINTER (0x83) 8: kind u8 (0 move, 1 down, 2 up)  9: buttons u8  10..11: x i16  12..13: y i16     surface-local coordinates
 *   CLOSE_REQUEST (0x84) no payload
 *
 * A message's length must match its type exactly: shorter is UIP_ETRUNC, longer is UIP_EBADLEN. Text is printable ASCII (0x20..0x7E) only.
 * Rectangles are validated against the surface and REJECTED, not clamped, when they do not fit.
 *
 * Identity: the caller (pid, generation, principal, capability) is supplied by the KERNEL, never read from a message. A surface belongs to the
 * process INSTANCE that created it (pid + generation + principal): a reused pid has a new generation and can never reach it. A handle that is
 * dead, stale, or belongs to another instance gets the same answer (UIP_ESTALE), so a client learns nothing about other clients' surfaces.
 *
 * The serial is 16 bits and never 0, so a slot's handle repeats only after 65535 reuses of that one slot. Even then ownership (pid, generation,
 * principal) is checked on every use, so only the SAME process instance could ever hit its own wrapped-around handle.
 *
 * Atomicity: SET_TEXT and SET_RECT only change the STAGED copy of a surface, and only after the whole message was validated. COMMIT copies the
 * staged copy to the visible one in one step. A message that fails validation changes nothing at all (tested with a state digest).
 */
#ifndef AURORA_UI_PROTO_H
#define AURORA_UI_PROTO_H
#include <stdint.h>

#define UIP_VERSION 1
#define UIP_MSG_MAX 64
#define UIP_HDR 8

enum {
    UIP_CREATE_SURFACE = 1, UIP_SET_TEXT = 2, UIP_SET_RECT = 3, UIP_COMMIT = 4, UIP_DESTROY_SURFACE = 5, UIP_REQUEST_FOCUS = 6,
    UIP_STATUS = 0x81, UIP_EVENT_KEY = 0x82, UIP_EVENT_POINTER = 0x83, UIP_CLOSE_REQUEST = 0x84
};

enum {
    UIP_OK = 0,
    UIP_EVERSION = -1,   /* wrong protocol version */
    UIP_ETYPE = -2,      /* unknown, or not a client-to-compositor type */
    UIP_ETRUNC = -3,     /* the message is shorter than its type requires */
    UIP_EBADLEN = -4,    /* longer than its type allows, or an oversized embedded length */
    UIP_EBADARG = -5,    /* an index, enum, rectangle or reserved field is out of range */
    UIP_ESTALE = -6,     /* no such surface for this caller (dead, stale, or someone else's: indistinguishable on purpose) */
    UIP_EQUOTA = -7,     /* too many surfaces (per client or in total) */
    UIP_EBUSY = -8,      /* too many unconsumed replies: the request is dropped, no reply is produced */
    UIP_EPERM = -9,      /* the caller holds no UI capability, or is not a valid process */
    UIP_EBADTEXT = -10,  /* text or title is not printable ASCII */
    UIP_EDUP = -11       /* the same CREATE (same sequence number) was repeated */
};

#define UIP_TITLE_MAX 24
#define UIP_TEXT_MAX 40
#define UIP_WIDGETS 16
#define UIP_STYLES 4
#define UIP_ROLES 4
#define UIP_MAX_SURFACES 8          /* in total */
#define UIP_PER_CLIENT 4            /* per process instance */
#define UIP_MAX_CLIENTS 8           /* = PROC_MAX; clients are indexed by pid 1..8 */
#define UIP_MAX_OUTSTANDING 2       /* unconsumed replies per client before further requests are dropped */
#define UIP_CHROME_H 28             /* the compositor draws a title strip of this height ABOVE every surface; clients never draw in it */
#define UIP_LAYER_CONTENT 0         /* client surfaces live ONLY in the content band: below the dock, corner, sheets, scrim, modals and toasts */

/* surface size classes: pixels */
#define UIP_SIZE_CLASSES 3
extern const uint16_t UIP_SIZE_W[UIP_SIZE_CLASSES], UIP_SIZE_H[UIP_SIZE_CLASSES];

/* ---- decoded form ---- */
struct uip_msg {
    uint8_t type;
    uint16_t seq;
    uint32_t handle;
    uint8_t a, b;               /* size class / widget; style or role */
    int16_t x, y;
    uint16_t w, h;
    uint8_t tlen;
    char text[UIP_TEXT_MAX + 1];    /* title or text, NUL-terminated */
};

/* Validate and decode one client message. Returns UIP_OK or an error; *out is filled only on UIP_OK. Pure: touches no state. */
int uip_decode(const uint8_t *buf, uint32_t len, struct uip_msg *out);

/* Encoders for messages the COMPOSITOR sends (the clients' side of the tests also uses them). Each returns the length written, or 0 if cap is too small. */
uint32_t uip_encode_status(uint8_t *buf, uint32_t cap, uint16_t seq, uint32_t handle, int16_t code, uint8_t req_type);
uint32_t uip_encode_key(uint8_t *buf, uint32_t cap, uint16_t seq, uint32_t handle, uint16_t code, uint16_t mods, uint8_t down, uint8_t repeat);
uint32_t uip_encode_pointer(uint8_t *buf, uint32_t cap, uint16_t seq, uint32_t handle, uint8_t kind, uint8_t buttons, int16_t x, int16_t y);
uint32_t uip_encode_close(uint8_t *buf, uint32_t cap, uint16_t seq, uint32_t handle);
/* Client-side request encoders (used by tests and, later, by EL0 clients). */
uint32_t uip_encode_create(uint8_t *buf, uint32_t cap, uint16_t seq, uint8_t size_class, const char *title);
uint32_t uip_encode_set_text(uint8_t *buf, uint32_t cap, uint16_t seq, uint32_t handle, uint8_t widget, uint8_t style, const char *text);
uint32_t uip_encode_set_rect(uint8_t *buf, uint32_t cap, uint16_t seq, uint32_t handle, uint8_t widget, uint8_t role, int16_t x, int16_t y, uint16_t w, uint16_t h);
uint32_t uip_encode_simple(uint8_t *buf, uint32_t cap, uint8_t type, uint16_t seq, uint32_t handle);   /* COMMIT, DESTROY_SURFACE, REQUEST_FOCUS */

/* ---- the compositor's model ---- */
struct uip_widget { uint8_t has_text, has_rect, style, role; int16_t x, y; uint16_t w, h; uint8_t tlen; char text[UIP_TEXT_MAX + 1]; };

struct uip_surface {
    uint8_t live;
    uint16_t serial;                /* changes every time the slot is reused; part of the handle */
    uint8_t owner_pid;
    uint64_t owner_gen;
    uint32_t owner_principal;
    uint8_t size_class;
    uint8_t tlen; char title[UIP_TITLE_MAX + 1];
    int16_t sx, sy;                 /* screen position: chosen by the COMPOSITOR (uip_place), never by the client */
    uint8_t placed;
    uint8_t focus_wished;           /* the client asked for focus (advisory) */
    uint32_t commits;
    struct uip_widget vis[UIP_WIDGETS];     /* what the compositor draws */
    struct uip_widget pend[UIP_WIDGETS];    /* staged by the client; becomes visible only at COMMIT */
};

struct uip_client { uint8_t used; uint64_t gen; uint32_t principal; uint8_t outstanding, nsurf, have_seq; uint16_t last_seq; };

struct uip_state {
    struct uip_surface s[UIP_MAX_SURFACES];
    struct uip_client c[UIP_MAX_CLIENTS];
    int8_t focus;                   /* slot of the focused surface, or -1: set only through uip_set_focus */
    int8_t z[UIP_MAX_SURFACES];     /* bottom to top */
    uint8_t nz;
    int16_t rx, ry, rw, rh;         /* the content region client surfaces must stay inside (compositor-chosen) */
    uint32_t evseq;
};

/* Identity of the sender, supplied by the kernel. can_ui is a capability granted by kernel code. */
struct uip_caller { uint8_t pid; uint64_t gen; uint32_t principal; uint8_t can_ui; };

void uip_init(struct uip_state *st, int16_t region_x, int16_t region_y, int16_t region_w, int16_t region_h);

/* Handle one message from a client: decode, authorise, validate, apply, and write the STATUS reply into reply (cap >= UIP_HDR + 3).
 * Returns the status code. *reply_len is the reply length, or 0 when no reply is produced (UIP_EBUSY). */
int uip_handle(struct uip_state *st, const struct uip_caller *who, const uint8_t *msg, uint32_t len, uint8_t *reply, uint32_t reply_cap, uint32_t *reply_len);

/* The client consumed one reply (the kernel calls this when the reply is received). Ignored for a stale instance. */
void uip_reply_consumed(struct uip_state *st, uint8_t pid, uint64_t gen);
/* The process instance exited or faulted: destroy all its surfaces and forget it. Ignored when the generation does not match the one on record. */
void uip_client_gone(struct uip_state *st, uint8_t pid, uint64_t gen);

/* ---- compositor-only controls: no client message reaches these ---- */
int  uip_place(struct uip_state *st, int slot, int16_t x, int16_t y);       /* the whole surface must lie inside the region; 0 or UIP_EBADARG / UIP_ESTALE */
int  uip_focus_owner(struct uip_state *st, uint8_t pid, uint64_t gen);    /* raise the topmost placed surface of this instance */
int  uip_set_focus(struct uip_state *st, int slot);                         /* -1 clears; raises the surface; 0 or UIP_ESTALE */
const struct uip_surface *uip_surface(const struct uip_state *st, int slot);
int  uip_slot_of(uint32_t handle);                                          /* slot encoded in a handle, or -1 */
uint32_t uip_handle_of(const struct uip_state *st, int slot);               /* the handle of a live slot, or 0 */
uint32_t uip_digest(const struct uip_state *st);                            /* FNV-1a over everything a client can influence on screen (tests, invariants) */
int  uip_check_invariants(const struct uip_state *st);                      /* 1 if the internal tables are consistent (tests) */

/* ---- event routing: who may receive what ---- */
struct uip_target { uint8_t pid; uint64_t gen; };
/* A key event goes only to the owner of the FOCUSED surface. Returns the message length (and *to), or 0 when there is no focus. */
uint32_t uip_route_key(struct uip_state *st, uint16_t code, uint16_t mods, uint8_t down, uint8_t repeat, struct uip_target *to, uint8_t *buf, uint32_t cap);
/* A pointer event goes only to the owner of the topmost PLACED surface under (x, y), in that surface's local coordinates. 0 if none. */
uint32_t uip_route_pointer(struct uip_state *st, int x, int y, uint8_t kind, uint8_t buttons, struct uip_target *to, uint8_t *buf, uint32_t cap);
/* The user asked to close the focused surface: a CLOSE_REQUEST for its owner. 0 when there is none. */
uint32_t uip_route_close(struct uip_state *st, struct uip_target *to, uint8_t *buf, uint32_t cap);
#endif
