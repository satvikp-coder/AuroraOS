/*
 * tests/programs/clock.c - the native Clock application, an unprivileged EL0 program (SYS_UI + SYS_TIMER).
 * Started only by the kernel launcher (kernel/proc/apps.c) as the installer-owned "clock.elf"
 * under the kernel-chosen identity 604. It holds the UI capability and the timer capability.
 *
 * What it does: shows uptime (monotonic ms since boot), offers preset countdown timers (1/5/25 min),
 * shows remaining time, delivers completion events, allows cancelling a running timer.
 * Keys: 1/2/3 select preset, S start, C cancel, Q quit. Esc/Tab never delivered to a client.
 * A CLOSE_REQUEST from the compositor also exits.
 * Every state change prints "clock ..." lines through SYS_WRITE (tagged "[user pN]" by the kernel).
 */
#include "common.h"
#define SYS_UI 41
#define SYS_TIMER 42
typedef unsigned char u8; typedef unsigned short u16;

#define TIMER_START 1
#define TIMER_CANCEL 2
#define TIMER_QUERY 3
#define TIMER_RELEASE 4
#define TIMER_NOW 5

static i64 tm(i64 op, i64 a, i64 b) { return sc(SYS_TIMER, op, a, b, 0, 0); }
static i64 ui_send(const void *m, u64 len, void *r, u64 rcap) { return sc(SYS_UI, 1, (i64)m, (i64)len, (i64)r, (i64)rcap); }
static i64 ui_poll(void *buf, u64 cap) { return sc(SYS_UI, 2, (i64)buf, (i64)cap, 0, 0); }

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 get32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void hdr(u8 *b, u8 type, u16 seq, u32 handle) { b[0] = 1; b[1] = type; put16(b + 2, seq); put32(b + 4, handle); }

static u16 useq; static u32 handle;

static i64 send_rect(u8 w, u8 role, int x, int y, int ww, int hh)
{
    u8 b[32], r[16]; hdr(b, 3, ++useq, handle); b[8] = w; b[9] = role; put16(b + 10, (u16)x); put16(b + 12, (u16)y); put16(b + 14, (u16)ww); put16(b + 16, (u16)hh);
    return ui_send(b, 18, r, sizeof r);
}
static i64 send_text(u8 w, u8 style, const char *t)
{
    u8 b[64], r[16]; u64 n = slen(t); if (n > 40) n = 40;
    hdr(b, 2, ++useq, handle); b[8] = w; b[9] = style; b[10] = (u8)n; for (u64 i = 0; i < n; i++) b[11 + i] = (u8)t[i];
    return ui_send(b, 11 + n, r, sizeof r);
}
static i64 commit(void) { u8 b[16], r[16]; hdr(b, 4, ++useq, handle); return ui_send(b, 8, r, sizeof r); }

/* ---------------------------------------------------------------- timer logic */
struct q { u8 state; u8 z[7]; u64 rem; };
static i64 query(i64 id, u8 *state, u64 *rem)
{
    struct q q; q.state = 0xEE; q.rem = 0xEEEEEEEEEEEEEEEEULL;
    i64 rc = tm(TIMER_QUERY, id, (i64)&q);
    if (rc == 0) { *state = q.state; *rem = q.rem; }
    return rc;
}

static const u32 PRESET_MS[3] = { 60000, 300000, 1500000 }; /* 1m, 5m, 25m */
static const char *const PRESET_NAME[3] = { "1 min", "5 min", "25 min" };

static void put_ms(char *d, u32 cap, u32 s)             /* m:ss (minutes may exceed 59) */
{
    char t[16]; u32 n = 0, m = s / 60; char dig[8]; int k = 0;
    do { dig[k++] = (char)('0' + m % 10); m /= 10; } while (m && k < 8);
    while (k) t[n++] = dig[--k];
    t[n++] = ':'; t[n++] = (char)('0' + (s % 60) / 10); t[n++] = (char)('0' + s % 10); t[n] = 0;
    u32 i = 0; while (i + 1 < cap && t[i]) { d[i] = t[i]; i++; } d[i] = 0;
}
static void put_hms(char *d, u32 cap, u32 s)            /* "Up h:mm:ss" */
{
    char t[24]; u32 n = 0, h = s / 3600, m = (s / 60) % 60; char dig[8]; int k = 0;
    t[n++] = 'U'; t[n++] = 'p'; t[n++] = ' ';
    do { dig[k++] = (char)('0' + h % 10); h /= 10; } while (h && k < 8);
    while (k) t[n++] = dig[--k];
    t[n++] = ':'; t[n++] = (char)('0' + m / 10); t[n++] = (char)('0' + m % 10); t[n++] = ':'; t[n++] = (char)('0' + (s % 60) / 10); t[n++] = (char)('0' + s % 10); t[n] = 0;
    u32 i = 0; while (i + 1 < cap && t[i]) { d[i] = t[i]; i++; } d[i] = 0;
}

static int timer_id = -1; static u32 timer_dur_s = 0; static u8 timer_preset = 0;
static u8 timer_state = 0; static char timer_val[16]; static char timer_up[24]; static char timer_live[64];
static u32 last_now_s = 0;

/* timer_state: 0=READY, 1=RUNNING, 2=DONE, 3=CANCELLED, 4=ERROR */
static void clk_update_live(void)
{
    char t[64]; const char *src = "";
    i64 now = tm(TIMER_NOW, 0, 0);
    u32 now_s = (now >= 0) ? (u32)(now / 1000) : 0;
    if (now_s != last_now_s) { last_now_s = now_s; put_hms(timer_up, sizeof timer_up, now_s); }

    switch (timer_state) {
    case 0: /* READY */
        { u32 n = 0; const char *p = "Ready. "; while (p[n] && n + 1 < (u32)sizeof t) { t[n] = p[n]; n++; } t[n] = 0; }
        { u32 n = 0; while (t[n]) n++; const char *p = PRESET_NAME[timer_preset % 3]; while (*p && n + 1 < (u32)sizeof t) { t[n++] = *p++; } t[n] = 0; }
        { u32 n = 0; while (t[n]) n++; const char *p = " selected. Press S to start."; while (*p && n + 1 < (u32)sizeof t) { t[n++] = *p++; } t[n] = 0; }
        src = t; break;
    case 1: /* RUNNING */ src = "Running. Press C to cancel."; break;
    case 2: /* DONE */ src = "Finished. Choose a time to run another."; break;
    case 3: /* CANCELLED */ src = "Cancelled. Nothing is running."; break;
    case 4: /* ERROR */ src = "Could not start: the service refused."; break;
    }
    u32 i = 0; while (src[i] && src[i] == timer_live[i]) i++;
    if (src[i] || timer_live[i]) { u32 n = 0; while (src[n] && n + 1 < sizeof timer_live) { timer_live[n] = src[n]; n++; } timer_live[n] = 0; }

    u32 show_s = PRESET_MS[timer_preset % 3] / 1000;
    if (timer_state == 1) {
        u8 st = 0; u64 rem = 0;
        if (query(timer_id, &st, &rem) == 0) {
            if (st == 1) show_s = (u32)((rem + 999) / 1000);
            else if (st == 2) { timer_state = 2; show_s = 0; }
            else if (st == 3) { timer_state = 3; show_s = 0; }
        }
    }
    put_ms(timer_val, sizeof timer_val, show_s);
}

static void refresh(void)
{
    clk_update_live();
    send_text(1, 0, timer_up);
    send_text(2, 1, timer_val);
    send_text(3, 0, timer_live);
    commit();
    { char l[80]; int n = 0; const char *p = "clock "; while (*p) l[n++] = *p++; for (const char *s = timer_live; *s && n < 78; s++) l[n++] = *s; l[n] = 0; say(l); }
}

static int on_key(u32 code, u32 mods)
{
    (void)mods;
    switch (code) {
    case 2: timer_preset = 0; refresh(); return 1;  /* 1 */
    case 3: timer_preset = 1; refresh(); return 1;  /* 2 */
    case 4: timer_preset = 2; refresh(); return 1;  /* 3 */
    case 31: /* S */
        if (timer_state == 0 || timer_state >= 2) {
            i64 id = tm(TIMER_START, PRESET_MS[timer_preset % 3], 0);
            if (id >= 0) { timer_id = (int)id; timer_dur_s = PRESET_MS[timer_preset % 3] / 1000; timer_state = 1; }
            else { timer_state = 4; }
            refresh();
        }
        return 1;
    case 46: /* C */
        if (timer_state == 1) {
            i64 rc = tm(TIMER_CANCEL, timer_id, 0);
            if (rc == 0) { tm(TIMER_RELEASE, timer_id, 0); timer_state = 3; timer_id = -1; timer_dur_s = 0; }
            else { timer_state = 4; }
            refresh();
        }
        return 1;
    case 16: /* Q */ return 2;
    default: return 0;
    }
}

void __attribute__((section(".text._start"))) _start(u64 mode)
{
    (void)mode;
    u8 b[48], r[16]; u64 n = slen("Clock");
    hdr(b, 1, ++useq, 0); b[8] = 2; b[9] = (u8)n; for (u64 i = 0; i < n; i++) b[10 + i] = (u8)"Clock"[i]; /* size class 2: large */
    i64 rc = ui_send(b, 10 + n, r, sizeof r);
    if (rc != 0) { sayn("clock create failed=", (u64)(-rc)); quit(2); }
    handle = get32(r + 4);

    send_rect(0, 0, 8, 8, 700, 24); send_text(0, 1, "Clock");
    send_rect(1, 2, 8, 36, 700, 20);  /* uptime */
    send_rect(2, 1, 8, 60, 700, 28);  /* timer value */
    send_rect(3, 0, 8, 92, 700, 20);  /* status line */
    send_rect(4, 2, 8, 120, 700, 20); send_text(4, 0, "1/2/3 choose   S start   C cancel   Q quit");
    say("clock started");
    timer_state = 0; timer_preset = 0; timer_id = -1; timer_dur_s = 0;
    clk_update_live(); refresh();

    for (;;) {
        u8 ev[16]; i64 got = ui_poll(ev, sizeof ev);
        if (got <= 0) { yield(); continue; }
        if (ev[1] == 0x84) { say("clock close requested"); quit(0); } /* CLOSE_REQUEST */
        if (ev[1] == 0x85) { /* TIMER event */
            u32 id = ev[8]; u8 st = ev[9];
            if ((int)id == timer_id && st == 2) { timer_state = 2; timer_dur_s = 0; }
            else if ((int)id == timer_id && st == 3) { timer_state = 3; timer_id = -1; timer_dur_s = 0; }
            refresh();
            continue;
        }
        if (ev[1] == 0x82 && ev[12]) { /* key press */
            u32 code = (u32)ev[8] | ((u32)ev[9] << 8), mods = (u32)ev[10] | ((u32)ev[11] << 8);
            int a = on_key(code, mods);
            if (a == 2) { say("clock exit"); quit(0); }
            if (a) refresh();
        }
    }
}