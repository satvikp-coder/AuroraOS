/*
 * kernel/input/input_core.c - pure input logic: no hardware, no allocation, no globals (host-tested under ASan/UBSan).
 * Layout policy: US QWERTY only, documented; no caps lock, no dead keys, no non-ASCII text. See input.h.
 */
#include <input.h>

/* Index = Linux key code 0..57. 0 = not a text key. Exactly 58 characters each (checked below). */
static const char NORMAL[] = "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`" "\0" "\\zxcvbnm,./" "\0\0\0" " ";
static const char SHIFTED[] = "\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~" "\0" "|ZXCVBNM<>?" "\0\0\0" " ";
_Static_assert(sizeof NORMAL == 59 && sizeof SHIFTED == 59, "keymap tables must cover key codes 0..57");

char input_us_char(uint16_t key, int shift)
{
    if (key >= 58) return 0;
    return shift ? SHIFTED[key] : NORMAL[key];
}

int32_t input_scale_axis(int32_t raw, int32_t min, int32_t max, uint32_t extent)
{
    if (max <= min || extent == 0) return 0;
    int64_t r = raw;
    if (r < min) r = min;
    if (r > max) r = max;
    int64_t span = (int64_t)max - min;                 /* 64-bit: no overflow for any int32 pair */
    return (int32_t)(((r - min) * (int64_t)(extent - 1)) / span);
}

void input_init(struct input_state *s, uint32_t width, uint32_t height)
{
    uint8_t *p = (uint8_t *)s;
    for (uint32_t i = 0; i < sizeof *s; i++) p[i] = 0;
    s->width = width; s->height = height;
    s->px = (int32_t)(width / 2); s->py = (int32_t)(height / 2);
}

int input_set_axis(struct input_state *s, uint32_t axis, int32_t min, int32_t max)
{
    if (axis > 1 || max <= min) return -1;
    s->ax[axis].min = min; s->ax[axis].max = max; s->ax[axis].valid = 1;
    return 0;
}

static int is_mod_key(uint16_t k) { return k == KEY_LEFTSHIFT || k == KEY_RIGHTSHIFT || k == KEY_LEFTCTRL || k == KEY_RIGHTCTRL || k == KEY_LEFTALT || k == KEY_RIGHTALT; }

int input_key_held(const struct input_state *s, uint16_t key)
{
    return key <= INPUT_MAX_KEY && (s->held[key >> 3] >> (key & 7)) & 1;
}

static void recompute_mods(struct input_state *s)
{
    uint16_t m = 0;
    if (input_key_held(s, KEY_LEFTSHIFT) || input_key_held(s, KEY_RIGHTSHIFT)) m |= MOD_SHIFT;
    if (input_key_held(s, KEY_LEFTCTRL) || input_key_held(s, KEY_RIGHTCTRL)) m |= MOD_CTRL;
    if (input_key_held(s, KEY_LEFTALT) || input_key_held(s, KEY_RIGHTALT)) m |= MOD_ALT;
    s->mods = m;
}

static void push(struct input_state *s, struct input_event e)
{
    if (s->count == INPUT_QUEUE_LEN) { s->st.dropped_out++; return; }   /* newest event dropped, never overwrite unread ones */
    s->q[(s->head + s->count) & (INPUT_QUEUE_LEN - 1)] = e;
    s->count++;
}

static struct input_event mk(const struct input_state *s, uint8_t kind, uint16_t key)
{
    struct input_event e = { kind, 0, key, s->mods, 0, s->px, s->py };
    return e;
}

int input_pop(struct input_state *s, struct input_event *out)
{
    if (!s->count) return 0;
    *out = s->q[s->head];
    s->head = (s->head + 1) & (INPUT_QUEUE_LEN - 1);
    s->count--;
    return 1;
}

void input_reset(struct input_state *s)
{
    for (uint32_t i = 0; i < sizeof s->held; i++) s->held[i] = 0;
    s->mods = 0; s->button = 0; s->pending_move = 0;
    s->st.resets++;
}

static void flush_move(struct input_state *s)
{
    if (!s->pending_move) return;
    s->pending_move = 0;
    int32_t nx = s->px, ny = s->py;
    if (s->have_raw[0]) nx = input_scale_axis(s->raw[0], s->ax[0].min, s->ax[0].max, s->width);
    if (s->have_raw[1]) ny = input_scale_axis(s->raw[1], s->ax[1].min, s->ax[1].max, s->height);
    if (nx != s->px || ny != s->py) { s->px = nx; s->py = ny; push(s, mk(s, IN_PTR_MOVE, 0)); }
}

static void on_key(struct input_state *s, uint16_t code, int32_t value)
{
    if (code > INPUT_MAX_KEY || value < 0 || value > 2) { s->st.ignored++; return; }
    if (code == BTN_LEFT) {
        flush_move(s);                                  /* a press happens where the pointer is NOW, not at the previous report */
        if (value == 1 && !s->button) { s->button = 1; push(s, mk(s, IN_PTR_DOWN, code)); }
        else if (value == 0 && s->button) { s->button = 0; push(s, mk(s, IN_PTR_UP, code)); }
        return;                                         /* repeat or duplicate: ignored */
    }
    if (code >= 0x100) { s->st.ignored++; return; }     /* other buttons: not supported yet */
    int was = input_key_held(s, code);
    if (value == 0) {
        if (!was) return;                               /* release without press: nothing to do */
        s->held[code >> 3] &= (uint8_t)~(1u << (code & 7));
        recompute_mods(s);
        push(s, mk(s, IN_KEY_UP, code));
        return;
    }
    if (value == 2 && !was) return;                     /* repeat of a key we never saw go down */
    s->held[code >> 3] |= (uint8_t)(1u << (code & 7));
    if (is_mod_key(code)) recompute_mods(s);
    struct input_event e = mk(s, IN_KEY_DOWN, code);
    e.repeat = (uint8_t)(value == 2);
    push(s, e);
    if (!is_mod_key(code) && !(s->mods & (MOD_CTRL | MOD_ALT))) {
        char c = input_us_char(code, !!(s->mods & MOD_SHIFT));
        if (c) { struct input_event t = mk(s, IN_TEXT, code); t.ch = (uint8_t)c; t.repeat = e.repeat; push(s, t); }
    }
}

static void on_abs(struct input_state *s, uint16_t code, int32_t value)
{
    if (code > ABS_Y || !s->ax[code].valid) { s->st.ignored++; return; }
    s->raw[code] = value; s->have_raw[code] = 1; s->pending_move = 1;
}

void input_feed(struct input_state *s, uint16_t type, uint16_t code, int32_t value)
{
    s->st.events_in++;
    switch (type) {
    case EV_KEY: on_key(s, code, value); break;
    case EV_ABS: on_abs(s, code, value); break;
    case EV_SYN:
        if (code == SYN_DROPPED) { input_reset(s); break; }          /* the device lost events: held state can no longer be trusted */
        if (code == SYN_REPORT) flush_move(s);
        break;
    default: s->st.ignored++; break;                                 /* EV_REL, EV_LED, EV_MSC, ...: not supported */
    }
}
