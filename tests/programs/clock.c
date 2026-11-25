/*
 * tests/programs/clock.c - the native Clock application, an unprivileged EL0 program (SYS_UI + SYS_TIMER).
 * Started only by the kernel launcher (kernel/proc/apps.c) as the installer-owned "clock.elf"
 * under the kernel-chosen identity 604. It holds the UI capability and a timer grant; it cannot
 * see another principal's files, open a review, or reach the framebuffer.
 *
 * A countdown clock: shows uptime (monotonic milliseconds since boot, from TIMER_NOW) and runs
 * one countdown timer at a time (1, 5 or 25 minutes). A preset is chosen with keys 1 / 5 / 2,
 * started with S, cancelled with C. Timer completion arrives as a UI event (type 0x85) posted by
 * the kernel; the app never polls the timer. Q quits. Esc and Tab are never delivered to a client
 * (the compositor keeps them to return focus to the shell). A CLOSE_REQUEST from the compositor
 * also exits.
 * Every state change prints one "clock ..." line through SYS_WRITE (tagged "[user pN]" by the
 * kernel) so a test can read exactly what this process shows and did.
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

struct q { u8 state; u8 z[7]; u64 rem; };   /* TIMER_QUERY reply: 16 bytes, state at 0, u64 remaining ms at 8 */

static i64 ui_send(const void *m, u64 len, void *r, u64 rcap) { return sc(SYS_UI, 1, (i64)m, (i64)len, (i64)r, (i64)rcap); }
static i64 ui_poll(void *buf, u64 cap) { return sc(SYS_UI, 2, (i64)buf, (i64)cap, 0, 0); }
static i64 tm(i64 op, i64 a, i64 b) { return sc(SYS_TIMER, op, a, b, 0, 0); }
static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 get32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void hdr(u8 *b, u8 type, u16 seq, u32 handle) { b[0] = 1; b[1] = type; put16(b + 2, seq); put32(b + 4, handle); }
static u16 seq; static u32 handle; static i64 timer_id = -1;

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

static void scpy(char *d, u32 cap, const char *s) { u32 i = 0; for (; i + 1 < cap && s[i]; i++) d[i] = s[i]; d[i] = 0; }

static const char *const PRESET_NAME[3] = { "1 min", "5 min", "25 min" };
static const u32 PRESET_S[3] = { 60, 300, 1500 };
static int preset = 0;          /* index into PRESET_* */
static int running = 0;         /* 1 while a timer is running */
static int done = 0;            /* 1 after the last timer finished */
static u32 rem_s = 0;           /* whole seconds left, rounded up */

static void put_ms(char *d, u32 cap, u32 s)              /* m:ss */
{
    char t[16]; u32 n = 0, m = s / 60; char dig[8]; int k = 0;
    do { dig[k++] = (char)('0' + m % 10); m /= 10; } while (m && k < 8);
    while (k) t[n++] = dig[--k];
    t[n++] = ':'; t[n++] = (char)('0' + (s % 60) / 10); t[n++] = (char)('0' + s % 10); t[n] = 0;
    { u32 i = 0; while (t[i] && i + 1 < cap) { d[i] = t[i]; i++; } d[i] = 0; }
}
static void put_hms(char *d, u32 cap, u32 s)            /* h:mm:ss */
{
    char t[24]; u32 n = 0, h = s / 3600, m = (s / 60) % 60; char dig[8]; int k = 0;
    do { dig[k++] = (char)('0' + h % 10); h /= 10; } while (h && k < 8);
    while (k) t[n++] = dig[--k];
    t[n++] = ':'; t[n++] = (char)('0' + m / 10); t[n++] = (char)('0' + m % 10); t[n++] = ':'; t[n++] = (char)('0' + (s % 60) / 10); t[n++] = (char)('0' + s % 10); t[n] = 0;
    { u32 i = 0; while (t[i] && i + 1 < cap) { d[i] = t[i]; i++; } d[i] = 0; }
}

static void status_text(char *out, u32 cap)
{
    if (running) { scpy(out, cap, "Running. C cancels."); return; }
    if (done) { scpy(out, cap, "Finished. Pick a time to run another."); return; }
    { u32 n = 0; const char *p = "Ready. "; while (p[n]) { out[n] = p[n]; n++; } const char *pn = PRESET_NAME[preset]; u32 k = 0; while (pn[k] && n + 1 < cap) { out[n] = pn[k]; n++; k++; } out[n] = 0; }
}

static void refresh(void)
{
    char up[16], val[12], st[64];
    i64 now = tm(TIMER_NOW, 0, 0);
    put_hms(up, sizeof up, now < 0 ? 0 : (u32)(now / 1000));
    if (running) put_ms(val, sizeof val, rem_s);
    else put_ms(val, sizeof val, done ? 0 : PRESET_S[preset]);
    status_text(st, sizeof st);
    send_text(1, 0, up); send_text(2, 1, val); send_text(3, 0, st); commit();
    say(up); say(val); say(st);
}

static void start_timer(void)
{
    if (running || timer_id >= 0) return;
    i64 id = tm(TIMER_START, (i64)(PRESET_S[preset] * 1000), 0);
    if (id < 0) { sayn("clock start refused rc=", (u64)(-id)); running = 0; done = 0; refresh(); return; }
    timer_id = id; running = 1; done = 0; rem_s = PRESET_S[preset];
    sayn("clock timer started id=", (u64)id); refresh();
}

static void cancel_timer(void)
{
    if (!running || timer_id < 0) return;
    i64 rc = tm(TIMER_CANCEL, timer_id, 0);
    if (rc == 0) { tm(TIMER_RELEASE, timer_id, 0); timer_id = -1; running = 0; done = 0; rem_s = 0; say("clock cancelled"); refresh(); }
    else { timer_id = -1; running = 0; done = 1; refresh(); sayn("clock cancel rc=", (u64)(-rc)); }
}

static void query_timer(void)
{
    if (!running || timer_id < 0) return;
    struct q q; q.state = 0xEE; q.rem = 0xEEEEEEEEEEEEEEEEULL;
    i64 rc = tm(TIMER_QUERY, timer_id, (i64)&q);
    if (rc != 0) { timer_id = -1; running = 0; done = 1; rem_s = 0; say("clock timer gone"); refresh(); return; }
    if (q.state == 2) { tm(TIMER_RELEASE, timer_id, 0); timer_id = -1; running = 0; done = 1; rem_s = 0; say("clock finished"); refresh(); return; }
    if (q.state == 3) { tm(TIMER_RELEASE, timer_id, 0); timer_id = -1; running = 0; done = 0; rem_s = 0; say("clock cancelled by service"); refresh(); return; }
    rem_s = (u32)((q.rem + 999u) / 1000u); if (rem_s > 86400u) rem_s = 86400u; refresh();
}

void __attribute__((section(".text._start"))) _start(u64 mode)
{
    (void)mode;
    u8 b[48], r[16]; u64 n = slen("Clock");
    hdr(b, 1, ++seq, 0); b[8] = 2; b[9] = (u8)n; for (u64 i = 0; i < n; i++) b[10 + i] = (u8)"Clock"[i];
    i64 rc = ui_send(b, 10 + n, r, sizeof r);
    if (rc != 0) { sayn("clock create failed=", (u64)(-rc)); quit(2); }
    handle = get32(r + 4);
    send_rect(0, 0, 8, 8, 700, 24); send_text(0, 1, "Clock");
    send_rect(1, 2, 8, 36, 700, 20);
    send_rect(2, 1, 8, 62, 700, 28);
    send_rect(3, 2, 8, 96, 700, 20);
    send_rect(4, 2, 8, 122, 700, 20); send_text(4, 0, "1 5 2 preset   S start   C cancel   Q quit");
    say("clock started");
    refresh();
    for (;;) {
        u8 ev[16]; i64 got = ui_poll(ev, sizeof ev);
        if (got <= 0) { yield(); continue; }
        if (ev[1] == 0x84) { say("clock close requested"); quit(0); }
        if (ev[1] == 0x85) { query_timer(); continue; }   /* timer completion: id in ev[8], state in ev[9] */
        if (ev[1] == 0x82 && ev[12]) {                    /* key down */
            u32 code = (u32)ev[8] | ((u32)ev[9] << 8);
            if (code == 16) { say("clock exit"); quit(0); }                       /* Q */
            if (code >= 2 && code <= 4) { preset = (int)(code - 2); done = 0; sayn("clock preset=", (u64)preset); refresh(); continue; }
            switch (code) {
            case 'S': case 's': start_timer(); break;
            case 'C': case 'c': cancel_timer(); break;
            default: break;
            }
        }
    }
}
