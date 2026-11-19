/*
 * tests/programs/calc.c - the native Calculator, an unprivileged EL0 application (SYS_UI). Started only by the kernel launcher (kernel/proc/apps.c) as the installer-owned "calc.elf"
 * under the kernel-chosen identity; it holds no other capability (no navigate right, no timer, no Passport grant) and cannot ask for any.
 * It draws one small surface through the UI protocol and serves its key events. Arithmetic is the pure, bounded state machine in calc_core.h.
 *   digits 0-9 (main row or keypad), + - * / (shift+= , -, shift+8, / and the keypad), Enter or = : equals, Backspace: delete, C or Delete: clear, Q: quit.
 * Esc and Tab are NOT delivered to a client: the compositor keeps them to return keyboard focus to the shell (a client can never trap the keyboard).
 * A CLOSE_REQUEST from the compositor also exits. Output through SYS_WRITE is tagged "[user pN]" by the kernel, so a log line proves what this process saw.
 */
#include "common.h"
#include "calc_core.h"
#define SYS_UI 41
typedef unsigned char u8; typedef unsigned short u16;

static i64 ui_send(const void *m, u64 len, void *r, u64 rcap) { return sc(SYS_UI, 1, (i64)m, (i64)len, (i64)r, (i64)rcap); }
static i64 ui_poll(void *buf, u64 cap) { return sc(SYS_UI, 2, (i64)buf, (i64)cap, 0, 0); }
static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 get32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void hdr(u8 *b, u8 type, u16 seq, u32 handle) { b[0] = 1; b[1] = type; put16(b + 2, seq); put32(b + 4, handle); }
static u16 seq; static u32 handle;

static i64 send_rect(u8 w, u8 role, int x, int y, int ww, int hh)
{
    u8 b[32], r[16]; hdr(b, 3, ++seq, handle); b[8] = w; b[9] = role; put16(b + 10, (u16)x); put16(b + 12, (u16)y); put16(b + 14, (u16)ww); put16(b + 16, (u16)hh);
    return ui_send(b, 18, r, sizeof r);
}
static i64 send_text(u8 w, u8 style, const char *t)
{
    u8 b[64], r[16]; u64 n = slen(t); if (n > 40) n = 40;
    hdr(b, 2, ++seq, handle); b[8] = w; b[9] = style; b[10] = (u8)n; for (u64 i = 0; i < n; i++) b[11 + i] = (u8)t[i];
    return ui_send(b, 11 + n, r, sizeof r);
}
static i64 commit(void) { u8 b[16], r[16]; hdr(b, 4, ++seq, handle); return ui_send(b, 8, r, sizeof r); }

static struct calc C;
static void refresh(void)
{
    char top[32], big[32];
    calc_pending(&C, top, sizeof top); calc_display(&C, big, sizeof big);
    send_text(1, 0, top[0] ? top : " "); send_text(2, 1, big); commit();
    char line[48]; const char *p = "calc display="; int n = 0; while (p[n]) { line[n] = p[n]; n++; }
    for (int i = 0; big[i] && n + 1 < (int)sizeof line; i++) line[n++] = big[i];
    line[n] = 0; say(line);                                        /* one line, so a test can read exactly what this process shows */
}

/* evdev code -> action; returns 1 if consumed */
static int on_key(u32 code, u32 mods)
{
    int shift = (mods & 1u) != 0;                     /* MOD_SHIFT (the input layer folds left and right Shift into one bit) */
    static const char DIG[] = { 0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0' };   /* KEY_1..KEY_0 = 2..11 */
    if (code >= 2 && code <= 11) { if (shift && code == 9) calc_op(&C, '*'); else calc_digit(&C, DIG[code] - '0'); return 1; }
    switch (code) {
    case 79: calc_digit(&C, 1); return 1; case 80: calc_digit(&C, 2); return 1; case 81: calc_digit(&C, 3); return 1;     /* keypad 1 2 3 */
    case 75: calc_digit(&C, 4); return 1; case 76: calc_digit(&C, 5); return 1; case 77: calc_digit(&C, 6); return 1;     /* 4 5 6 */
    case 71: calc_digit(&C, 7); return 1; case 72: calc_digit(&C, 8); return 1; case 73: calc_digit(&C, 9); return 1;     /* 7 8 9 */
    case 82: calc_digit(&C, 0); return 1;                                                                                  /* 0 */
    case 78: calc_op(&C, '+'); return 1; case 74: calc_op(&C, '-'); return 1; case 55: calc_op(&C, '*'); return 1; case 98: calc_op(&C, '/'); return 1;
    case 13: if (shift) calc_op(&C, '+'); else calc_equals(&C); return 1;          /* "=" key; shift+= is "+" */
    case 12: calc_op(&C, '-'); return 1;
    case 53: calc_op(&C, '/'); return 1;
    case 28: case 96: calc_equals(&C); return 1;                                   /* Enter, keypad Enter */
    case 14: calc_backspace(&C); return 1;
    case 46: case 111: calc_clear(&C); return 1;                                   /* C, Delete */
    default: return 0;
    }
}

void __attribute__((section(".text._start"))) _start(u64 mode)
{
    (void)mode;
    u8 b[48], r[16]; u64 n = slen("Calculator");
    hdr(b, 1, ++seq, 0); b[8] = 0; b[9] = (u8)n; for (u64 i = 0; i < n; i++) b[10 + i] = (u8)"Calculator"[i];
    i64 rc = ui_send(b, 10 + n, r, sizeof r);
    if (rc != 0) { sayn("calc create failed=", (u64)(-rc)); quit(2); }
    handle = get32(r + 4);
    send_rect(0, 0, 8, 8, 240, 24); send_text(0, 1, "Calculator");
    send_rect(1, 2, 8, 36, 240, 20); send_text(1, 0, " ");
    send_rect(2, 1, 8, 60, 240, 28); calc_clear(&C);
    say("calc started"); refresh();
    for (;;) {
        u8 ev[16]; i64 got = ui_poll(ev, sizeof ev);
        if (got <= 0) { yield(); continue; }
        if (ev[1] == 0x84) { say("calc close requested"); quit(0); }
        if (ev[1] == 0x82 && ev[12]) {                                             /* a key press (not a release) */
            u32 code = (u32)ev[8] | ((u32)ev[9] << 8), mods = (u32)ev[10] | ((u32)ev[11] << 8);
            if (code == 16) { say("calc exit"); quit(0); }                         /* Q */
            if (on_key(code, mods)) refresh();
        }
    }
}
