/*
 * kernel/include/input.h - native input (issue #7, stage G5-A/C): evdev-style raw events from virtio-input, normalised events.
 *
 * Layers (each independent of the one above it):
 *   1. kernel/drivers/virtio_input.c  device transport: finds virtio-input devices, reads their capabilities, delivers RAW events.
 *   2. kernel/input/input_core.c      pure logic (no hardware, host-tested): modifier state, US-layout text, pointer scaling,
 *                                     a bounded queue of NORMALISED events.
 * A key event is a physical key (Linux evdev code), never a character. TEXT events are produced separately, only for the
 * documented US layout and only without Ctrl/Alt. All device data is untrusted: unknown types/codes are ignored and counted.
 */
#ifndef AURORA_INPUT_H
#define AURORA_INPUT_H
#include <stdint.h>

/* Linux evdev constants used by the virtio-input protocol (VIRTIO 1.x section 5.8). */
#define EV_SYN 0u
#define EV_KEY 1u
#define EV_REL 2u
#define EV_ABS 3u
#define SYN_REPORT 0u
#define SYN_DROPPED 3u
#define ABS_X 0u
#define ABS_Y 1u
#define KEY_ESC 1u
#define KEY_BACKSPACE 14u
#define KEY_TAB 15u
#define KEY_ENTER 28u
#define KEY_LEFTCTRL 29u
#define KEY_LEFTSHIFT 42u
#define KEY_RIGHTSHIFT 54u
#define KEY_LEFTALT 56u
#define KEY_SPACE 57u
#define KEY_RIGHTCTRL 97u
#define KEY_RIGHTALT 100u
#define KEY_HOME 102u
#define KEY_UP 103u
#define KEY_LEFT 105u
#define KEY_RIGHT 106u
#define KEY_END 107u
#define KEY_DOWN 108u
#define KEY_DELETE 111u
#define BTN_LEFT 0x110u
#define INPUT_MAX_KEY 0x2ffu           /* codes above are ignored */

#define MOD_SHIFT 1u
#define MOD_CTRL 2u
#define MOD_ALT 4u

enum input_kind { IN_NONE = 0, IN_KEY_DOWN, IN_KEY_UP, IN_TEXT, IN_PTR_MOVE, IN_PTR_DOWN, IN_PTR_UP };

/* A normalised event. key: evdev code (KEY_*, BTN_*). ch: printable ASCII for IN_TEXT, else 0. x,y: pointer position in pixels. */
struct input_event {
    uint8_t kind;
    uint8_t repeat;                    /* 1 for a device auto-repeat key-down */
    uint16_t key;
    uint16_t mods;                     /* MOD_* in effect when the event happened */
    uint8_t ch;
    int32_t x, y;
};

#define INPUT_QUEUE_LEN 64u            /* power of two */

struct input_axis { int32_t min, max; uint8_t valid; };

struct input_stats { uint64_t events_in, ignored, dropped_out, resets; };

struct input_state {
    uint32_t width, height;            /* pointer clamp range: [0,width-1] x [0,height-1] */
    struct input_axis ax[2];
    int32_t raw[2], px, py;
    uint8_t have_raw[2], pending_move, button;
    uint8_t held[(INPUT_MAX_KEY + 8u) / 8u];   /* bitmap of keys currently down */
    uint16_t mods;
    struct input_event q[INPUT_QUEUE_LEN];
    uint32_t head, count;
    struct input_stats st;
};

void input_init(struct input_state *s, uint32_t width, uint32_t height);
/* Declare a pointer axis range (ABS_X or ABS_Y). Returns 0, or -1 for an unusable range (max <= min) or axis. */
int input_set_axis(struct input_state *s, uint32_t axis, int32_t min, int32_t max);
/* Feed one RAW event. Never fails: bad input is ignored and counted. */
void input_feed(struct input_state *s, uint16_t type, uint16_t code, int32_t value);
/* Forget all held keys/buttons (device reset or lost events); queued events are kept. */
void input_reset(struct input_state *s);
int  input_pop(struct input_state *s, struct input_event *out);   /* 1 = event returned, 0 = empty */
/* Pure helpers (exposed for tests). */
int32_t input_scale_axis(int32_t raw, int32_t min, int32_t max, uint32_t extent);
char input_us_char(uint16_t key, int shift);    /* 0 = not a text key */
int  input_key_held(const struct input_state *s, uint16_t key);

/* ---- virtio-input device driver (kernel/drivers/virtio_input.c) ---- */
#define VINPUT_MAX_DEVICES 4

struct vinput_info { int devices, keyboards, pointers, failed; uint64_t polled_events, bad_used; };

/* Probe every virtio-mmio node from the device tree (modern transport only). Absent devices are a normal result (returns 0).
 * Pointer axis ranges read from a tablet are written into `s`. Returns the number of devices brought up, or a negative value
 * if the transport window itself could not be mapped. Idempotent. */
int  vinput_init(const void *fdt, struct input_state *s);
/* Drain completed device buffers (bounded work per call, never blocks) and feed them to `s`. Returns events consumed. */
int  vinput_poll(struct input_state *s);
const struct vinput_info *vinput_get_info(void);

#endif
