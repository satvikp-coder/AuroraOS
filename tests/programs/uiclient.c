/*
 * tests/programs/uiclient.c - EL0 UI clients (SYS_UI = 41) for tests/qemu/ui-client-tests.sh. The kernel launch registry passes the mode in x0:
 *   1 DEMO1    creates a surface, shows a label and a key counter, then serves its events: every key press updates the counter
 *   2 DEMO2    the same with other text (it must never receive input meant for DEMO1)
 *   3 HOSTILE  tries to do everything a malicious client could with the interface and prints what the kernel answered, then floods the interface
 *              with invalid messages and focus requests forever
 *   4 FAULTER  draws a surface, then dereferences address 0 after about 300 ticks
 *   5 NOCAP    has no UI capability: its CREATE must be refused
 * Output goes through SYS_WRITE (the kernel tags each line "[user pN] "), so a log line proves what the process saw at EL0.
 */
#include "common.h"
#define SYS_GETPID 3
#define SYS_TICKS 5
#define SYS_UI 41

static i64 ui_send(const void *m, u64 len, void *r, u64 rcap) { return sc(SYS_UI, 1, (i64)m, (i64)len, (i64)r, (i64)rcap); }
static i64 ui_poll(void *buf, u64 cap) { return sc(SYS_UI, 2, (i64)buf, (i64)cap, 0, 0); }
typedef unsigned char u8; typedef unsigned short u16; typedef short i16_;

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 get32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void hdr(u8 *b, u8 type, u16 seq, u32 handle) { b[0] = 1; b[1] = type; put16(b + 2, seq); put32(b + 4, handle); }

static u64 mk_create(u8 *b, u16 seq, u8 size_class, const char *title)
{
    u64 n = slen(title);
    hdr(b, 1, seq, 0); b[8] = size_class; b[9] = (u8)n;
    for (u64 i = 0; i < n; i++) b[10 + i] = (u8)title[i];
    return 10 + n;
}
static u64 mk_text(u8 *b, u16 seq, u32 h, u8 w, u8 style, const char *t)
{
    u64 n = slen(t);
    hdr(b, 2, seq, h); b[8] = w; b[9] = style; b[10] = (u8)n;
    for (u64 i = 0; i < n; i++) b[11 + i] = (u8)t[i];
    return 11 + n;
}
static u64 mk_rect(u8 *b, u16 seq, u32 h, u8 w, u8 role, int x, int y, int ww, int hh)
{
    hdr(b, 3, seq, h); b[8] = w; b[9] = role; put16(b + 10, (u16)x); put16(b + 12, (u16)y); put16(b + 14, (u16)ww); put16(b + 16, (u16)hh);
    return 18;
}
static u64 mk_simple(u8 *b, u8 type, u16 seq, u32 h) { hdr(b, type, seq, h); return 8; }

static u16 seq;
static i64 send_simple(u8 type, u32 h) { u8 b[64], r[16]; u64 n = mk_simple(b, type, ++seq, h); return ui_send(b, n, r, sizeof r); }
static i64 send_text(u32 h, u8 w, u8 style, const char *t) { u8 b[64], r[16]; u64 n = mk_text(b, ++seq, h, w, style, t); return ui_send(b, n, r, sizeof r); }
static i64 send_rect(u32 h, u8 w, u8 role, int x, int y, int ww, int hh) { u8 b[64], r[16]; u64 n = mk_rect(b, ++seq, h, w, role, x, y, ww, hh); return ui_send(b, n, r, sizeof r); }
static i64 create(u8 size_class, const char *title, u32 *handle)
{
    u8 b[64], r[16]; u64 n = mk_create(b, ++seq, size_class, title);
    i64 rc = ui_send(b, n, r, sizeof r);
    if (rc == 0 && handle) *handle = get32(r + 4);
    return rc;
}
static void sayi(const char *s, i64 v)                 /* "<s><signed decimal>" */
{
    char b[96]; u64 n = 0; while (s[n] && n < 60) { b[n] = s[n]; n++; }
    u64 u = v < 0 ? (u64)(-v) : (u64)v; if (v < 0) b[n++] = '-';
    char d[24]; int k = 0; if (!u) d[k++] = '0'; while (u) { d[k++] = (char)('0' + u % 10); u /= 10; }
    while (k) b[n++] = d[--k];
    b[n] = 0; say(b);
}
static u64 ticks(void) { return (u64)sc(SYS_TICKS, 0, 0, 0, 0, 0); }
static void burn(int n) { for (int i = 0; i < n; i++) __asm__ volatile ("" ::: "memory"); }

static void counter_text(char *out, u64 v)             /* "keys: <v>" */
{
    const char *p = "keys: "; u64 n = 0; while (p[n]) { out[n] = p[n]; n++; }
    char d[24]; int k = 0; if (!v) d[k++] = '0'; while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) out[n++] = d[--k];
    out[n] = 0;
}

static int bad_count, good_count;
static void expect(const char *name, i64 got, i64 want)
{
    if (got == want) { good_count++; sayi(name, got); }
    else { bad_count++; say("hostile BAD: "); sayi(name, got); }
}

void __attribute__((section(".text._start"))) _start(u64 mode)
{
    u32 h = 0;
    if (mode == 5) {
        i64 rc = create(0, "nocap", &h);
        sayi("nocap create=", rc);                     /* -309: the kernel never granted this identity the UI capability */
        quit(0);
    }
    if (mode == 1 || mode == 2 || mode == 4) {
        i64 rc = create(0, mode == 2 ? "second" : "demo", &h);
        if (rc != 0) { sayi("create failed=", rc); quit(2); }
        send_rect(h, 0, 0, 8, 8, 240, 24);  send_text(h, 0, 1, mode == 2 ? "Second client" : mode == 4 ? "I will crash" : "Hello from EL0");
        send_rect(h, 1, 1, 8, 40, 240, 24); send_text(h, 1, 0, "keys: 0");
        send_rect(h, 2, 0, 8, 76, 240, 24); send_text(h, 2, 2, "drawn by an EL0 process");
        i64 c = send_simple(4, h);
        sayi("surface committed=", c);
        if (mode == 4) {
            u64 t0 = ticks();
            while (ticks() - t0 < 300) burn(5000);
            say("fault now");
            *(volatile u64 *)0 = 1;
            quit(99);
        }
        u64 keys = 0; char txt[32];
        for (;;) {
            u8 ev[16]; i64 n = ui_poll(ev, sizeof ev);
            if (n <= 0) { yield(); continue; }
            u8 type = ev[1];
            if (type == 0x82) {                                         /* EVENT_KEY */
                u32 code = (u32)ev[8] | ((u32)ev[9] << 8);
                if (ev[12]) { sayi("ev key down code=", (i64)code); keys++; counter_text(txt, keys); send_text(h, 1, 0, txt); send_simple(4, h); }
                else sayi("ev key up code=", (i64)code);
            } else if (type == 0x83) {                                  /* EVENT_POINTER */
                sayi("ev ptr kind=", ev[8]); sayi("ev ptr x=", (i16_)((u16)ev[10] | ((u16)ev[11] << 8))); sayi("ev ptr y=", (i16_)((u16)ev[12] | ((u16)ev[13] << 8)));
            } else if (type == 0x84) say("ev close");
        }
    }
    if (mode == 3) {
        i64 rc = create(0, "hostile", &h);
        expect("hostile own surface=", rc, 0);
        u8 b[80], r[16]; u64 n;
        n = mk_create(b, ++seq, 0, "x"); b[0] = 2;                       expect("hostile wrong version=", ui_send(b, n, r, sizeof r), -301);
        n = mk_simple(b, 0x81, ++seq, h);                                expect("hostile compositor-only type (STATUS)=", ui_send(b, n, r, sizeof r), -302);
        n = mk_simple(b, 4, ++seq, h);                                   expect("hostile truncated message=", ui_send(b, 5, r, sizeof r), -303);
        n = mk_text(b, ++seq, h, 0, 0, "a"); b[10] = 41;                 expect("hostile oversized text length=", ui_send(b, n, r, sizeof r), -304);
        n = mk_text(b, ++seq, h, 16, 0, "a");                            expect("hostile widget index 16=", ui_send(b, n, r, sizeof r), -305);
        expect("hostile rectangle beyond the surface=", send_rect(h, 0, 0, 250, 0, 50, 20), -305);
        expect("hostile zero-size rectangle=", send_rect(h, 0, 0, 0, 0, 0, 20), -305);
        { u32 hh; expect("hostile size class 3=", create(3, "x", &hh), -305); }
        { char t[4] = { 'a', (char)0xC3, 'b', 0 }; u32 hh; expect("hostile non-ASCII title=", create(0, t, &hh), -310); }
        { int accepted = 0, rejected = 0;                                  /* guess every handle of the first serial: only the client's own may work */
          for (u32 k = 0; k < 8; k++) { i64 g = send_text(0x100 + k, 3, 0, "pwned"); if (g == 0) accepted++; else if (g == -306) rejected++; }
          sayi("hostile foreign handles accepted=", accepted); sayi("hostile foreign handles rejected=", rejected);
          expect("hostile exactly one guessed handle (its own) worked=", accepted, 1); }
        expect("hostile advisory focus request=", send_simple(6, h), 0);
        { int made = 1; u32 hh; i64 last = 0;
          for (int i = 0; i < 6; i++) { last = create(0, "q", &hh); if (last == 0) made++; else break; }
          sayi("hostile surfaces created=", made); expect("hostile quota refusal=", last, -307); }
        expect("hostile poll into a null buffer=", ui_poll((void *)0, 16), -2);
        expect("hostile send from a kernel address=", ui_send((const void *)0xFFFF000000000000UL, 16, r, sizeof r), -2);
        expect("hostile send of 65 bytes=", ui_send(b, 65, r, sizeof r), -3);
        expect("hostile unknown operation=", sc(SYS_UI, 3, 0, 0, 0, 0), -1);
        sayi("hostile summary good=", good_count); sayi("hostile summary bad=", bad_count);
        for (u64 i = 0;; i++) {                                           /* flood: invalid messages and focus requests, forever */
            n = mk_create(b, ++seq, 0, "x"); b[0] = (u8)(2 + (i & 7));
            ui_send(b, n, r, sizeof r);
            send_simple(6, h);
            if ((i & 63) == 0) yield();
            if (i % 20000 == 0) sayi("hostile flood iterations=", (i64)i);
        }
    }
    quit(50);
}
