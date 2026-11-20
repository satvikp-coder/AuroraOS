/*
 * tests/programs/files.c - the native Files viewer, an unprivileged EL0 program (SYS_UI + SYS_FS). Started only by the kernel launcher (kernel/proc/apps.c) as the installer-owned
 * "files.elf" under the kernel-chosen identity 603. It holds only the UI capability.
 *
 * What it does: lists the files AuroraFS lets THIS identity read (SYS_FS LIST: files of other principals that are not readable by it do not exist for it, so they are neither listed nor
 * guessable), shows name, size and who owns it (in words), and previews the first 144 bytes of the selected file as text when every byte is printable ASCII or newline; otherwise it
 * reports "binary" and shows no byte of the file. It is READ-ONLY BY CONSTRUCTION OF THE PROGRAM: it calls only LIST (6) and READ (3), never create/write/delete/set-permission/stat-by-guess;
 * what it can see or read is still decided by the kernel's permission checks, not by this program. SYS_FS LIST returns at most 16 entries, so the view says when it is showing only the first 16.
 *
 * Keys: Up/Down select, R refresh, Q quit. Esc and Tab are never delivered to a client. A CLOSE_REQUEST also exits.
 * Every state change prints "files ..." lines through SYS_WRITE (tagged "[user pN]" by the kernel) so a test can read exactly what this process shows.
 */
#include "common.h"
#include "files_core.h"
#define SYS_UI 41
typedef unsigned char u8; typedef unsigned short u16;
#define FILES_PRINCIPAL 603u

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

/* ---------------------------------------------------------------- state */
static struct fent ents[F_MAX];
static struct fview V;
static const char *status = "";
static char pv[F_PREVIEW_LINES][F_COLS + 4]; static int pv_kind, pv_used;
static const char *pv_msg = "";

static const char *fs_why(i64 rc)
{
    if (rc == -115 || rc == -111 || rc == -114) return "No disk";
    if (rc == -112) return "Disk error";
    return "Storage error";
}

static void load_preview(void)
{
    static u8 raw[F_PREVIEW_BYTES + 8];
    pv_kind = F_P_EMPTY; pv_used = 0; pv_msg = ""; pv[0][0] = 0;
    for (int i = 0; i < F_PREVIEW_LINES; i++) pv[i][0] = 0;
    if (V.n == 0) return;
    const struct fent *e = &ents[V.sel];
    i64 got = sc(SYS_FS, 3, (i64)e->id, 0, (i64)raw, F_PREVIEW_BYTES);               /* READ the first bytes only */
    if (got < 0) { pv_kind = -1; pv_msg = got == -107 ? "Not allowed to read this file" : (got == -108 ? "File changed or removed: press R" : "Could not read this file"); sayn("files preview error rc=", (u64)(-got)); return; }
    pv_kind = fc_preview(raw, (int)got, e->size, pv, &pv_used);
    if (pv_kind == F_P_EMPTY) pv_msg = "(empty file)";
    else if (pv_kind == F_P_BINARY) pv_msg = "(binary file: no preview)";
}

static void log_state(void)
{
    char l[64]; int n = 0; const char *p = "files sel=";
    while (p[n]) { l[n] = p[n]; n++; }
    if (V.n) { const struct fent *e = &ents[V.sel]; for (int i = 0; i < F_NAME && e->name[i] && n < 60; i++) { fc_u8 c = (fc_u8)e->name[i]; l[n++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?'; } }
    l[n] = 0; say(l);
    if (pv_kind == F_P_TEXT) { for (int i = 0; i < pv_used; i++) { char b[80]; int m = 0; const char *q = "files preview line="; while (q[m]) { b[m] = q[m]; m++; } for (int k = 0; pv[i][k] && m < 78; k++) b[m++] = pv[i][k]; b[m] = 0; say(b); } }
    else if (pv_kind == F_P_EMPTY) say("files preview empty");
    else if (pv_kind == F_P_BINARY) say("files preview binary");
    else say("files preview error");
}

static void refresh_ui(void)
{
    char row[44];
    const char *st = status;
    if (!*st) st = V.n == 0 ? "No files you can read" : (V.n >= F_MAX ? "Showing the first 16 files" : "Ready");
    send_text(1, 0, st);
    for (int i = 0; i < F_ROWS; i++) {
        int idx = V.top + i;
        if (idx < V.n) { fc_row(row, sizeof row, &ents[idx], idx == V.sel, FILES_PRINCIPAL); send_text((u8)(2 + i), idx == V.sel ? 1 : 0, row); }
        else send_text((u8)(2 + i), 0, " ");
    }
    for (int i = 0; i < F_PREVIEW_LINES; i++) {
        const char *t = " ";
        if (pv_kind == F_P_TEXT) { if (i < pv_used && pv[i][0]) t = pv[i]; }
        else if (i == 0 && pv_msg[0]) t = pv_msg;
        send_text((u8)(11 + i), 0, t);
    }
    commit(); log_state();
    { char b[64]; int m = 0; const char *q = "files status="; while (q[m]) { b[m] = q[m]; m++; } for (int k = 0; st[k] && m < 62; k++) b[m++] = st[k]; b[m] = 0; say(b); }
}

static void load_list(void)
{
    i64 rc = sc(SYS_FS, 6, (i64)ents, F_MAX, 0, 0);
    if (rc < 0) { fv_init(&V, 0); status = fs_why(rc); sayn("files list failed rc=", (u64)(-rc)); load_preview(); return; }
    status = "";
    fv_init(&V, (int)rc);
    sayn("files list n=", (u64)V.n);
    for (int i = 0; i < V.n; i++) { char l[64]; fc_row(l, sizeof l, &ents[i], 0, FILES_PRINCIPAL); char b[80]; int m = 0; const char *q = "files row="; while (q[m]) { b[m] = q[m]; m++; } for (int k = 0; l[k] && m < 78; k++) b[m++] = l[k]; b[m] = 0; say(b); }
    load_preview();
}

void __attribute__((section(".text._start"))) _start(u64 mode)
{
    (void)mode;
    u8 b[48], r[16]; u64 n = slen("Files");
    hdr(b, 1, ++useq, 0); b[8] = 2; b[9] = (u8)n; for (u64 i = 0; i < n; i++) b[10 + i] = (u8)"Files"[i];     /* size class 2: large */
    i64 rc = ui_send(b, 10 + n, r, sizeof r);
    if (rc != 0) { sayn("files create failed=", (u64)(-rc)); quit(2); }
    handle = get32(r + 4);
    send_rect(0, 0, 8, 8, 700, 24); send_text(0, 1, "Files (read only)");
    send_rect(1, 2, 8, 36, 700, 20);
    for (int i = 0; i < F_ROWS; i++) send_rect((u8)(2 + i), 1, 8, 62 + i * 26, 700, 24);
    send_rect(10, 2, 8, 272, 700, 20); send_text(10, 0, "Preview");
    for (int i = 0; i < F_PREVIEW_LINES; i++) send_rect((u8)(11 + i), 1, 8, 294 + i * 22, 700, 22);
    send_rect(15, 2, 8, 390, 700, 20); send_text(15, 0, "Up/Down select   R refresh   Q quit");
    say("files started");
    load_list(); refresh_ui();
    for (;;) {
        u8 ev[16]; i64 got = ui_poll(ev, sizeof ev);
        if (got <= 0) { yield(); continue; }
        if (ev[1] == 0x84) { say("files close requested"); quit(0); }
        if (ev[1] == 0x82 && ev[12]) {                                             /* a key press (not a release) */
            u32 code = (u32)ev[8] | ((u32)ev[9] << 8);
            if (code == 16) { say("files exit"); quit(0); }                        /* Q */
            if (code == 19) { load_list(); refresh_ui(); }                         /* R */
            else if (code == 103 || code == 108) { if (fv_move(&V, code == 103 ? -1 : 1)) { load_preview(); refresh_ui(); } }
        }
    }
}
