/*
 * tests/programs/notes.c - the native Notes application, an unprivileged EL0 program (SYS_UI + SYS_FS). Started only by the kernel launcher (kernel/proc/apps.c) as the
 * installer-owned "notes.elf" under the kernel-chosen identity 602. It holds no capability beyond the UI one and its own private files; it cannot name another principal's
 * files (AuroraFS authorizes by the kernel-supplied principal), open a review, or reach the framebuffer.
 *
 * One note, kept in two alternating slot files ("note.a", "note.b", owner-private perm 3) in the format of notes_core.h. A save writes the slot that does NOT hold the current
 * note, reads it back through the filesystem, decodes it and compares it with the editor buffer; only then does the status say "Saved". The previous note survives any failed
 * or torn write. A damaged note or one written by a NEWER version is never overwritten: saving is switched off and the status says why.
 *
 * Keys: printable ASCII, Enter (new line), Backspace, Delete, arrows, Home, End; Ctrl+S saves; Ctrl+Q quits (Q alone types). Esc and Tab are never delivered to a client (the compositor
 * keeps them to return focus to the shell). A CLOSE_REQUEST from the compositor also exits (without saving: the status line always shows whether the note is saved).
 * Every state change prints one "notes ..." line through SYS_WRITE (tagged "[user pN]" by the kernel) so a test can read exactly what this process shows and did.
 */
#include "common.h"
#include "notes_core.h"
#define SYS_UI 41
typedef unsigned char u8; typedef unsigned short u16;

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

/* ---------------------------------------------------------------- storage */
#define AE_FS(x) (-100 + (x))                      /* AE_FS_BASE + FS_E* */
static const char *const SLOT[2] = { "note.a", "note.b" };
enum { SL_EMPTY, SL_VALID, SL_BAD, SL_NEWER };
static struct note N;                              /* the editor buffer */
static int cur_slot = -1;                          /* slot holding the note that is on disk now, -1 none */
static u32 cur_seq;
static int dirty, locked, nostore;                 /* unsaved edits; saving switched off (damaged / newer); storage unavailable at load time */
static const char *status = "";
static void ncopy(struct note *d, const struct note *s) { for (int i = 0; i < s->len; i++) d->buf[i] = s->buf[i]; d->len = s->len; d->cur = s->cur; }   /* no struct assignment: it would need memcpy, which this freestanding program does not link */

static const char *fs_why(i64 rc)                  /* a short honest reason for a failed storage call */
{
    if (rc == AE_FS(-16)) return "Disk is read-only";
    if (rc == AE_FS(-4) || rc == AE_FS(-5) || rc == AE_FS(-18)) return "Disk or quota is full";   /* no space, no free file slot (all 32 used), or this app's own quota */
    if (rc == AE_FS(-15) || rc == AE_FS(-11) || rc == AE_FS(-14)) return "No disk";
    if (rc == AE_FS(-17)) return "Could not confirm the write";
    if (rc == AE_FS(-12)) return "Disk error";
    if (rc == AE_FS(-7)) return "Not allowed";
    return "Storage error";
}

/* fs_lookup returns the caller's own file of that name first, else the first file of that name the caller may see. Notes only ever uses files it OWNS: a visible file of another
 * principal is treated as absent (so it is neither read nor written, and our own file is created next to it). 602 is this application's kernel-assigned identity. */
#define NOTES_PRINCIPAL 602u
struct ustat { u64 id; u32 size, owner, perm, reserved; char name[32]; };
static i64 lookup_own(const char *name)
{
    i64 id = fs_lookup(name);
    if (id < 0) return id;
    struct ustat st; i64 rc = sc(SYS_FS, 5, id, (i64)&st, 0, 0);
    if (rc < 0) return rc;
    return st.owner == NOTES_PRINCIPAL ? id : EFS_ENOENT;
}

/* Read one slot into n (a scratch note). *seq set when valid. */
static int read_slot(int s, struct note *n, u32 *seq, i64 *err)
{
    static u8 raw[NOTE_FILE_MAX + 16];
    i64 id = lookup_own(SLOT[s]);
    *err = 0;
    if (id == EFS_ENOENT) return SL_EMPTY;
    if (id < 0) { *err = id; return SL_BAD; }
    i64 got = fs_read(id, 0, raw, sizeof raw);
    if (got < 0) { *err = got; return SL_BAD; }
    if (got == 0) return SL_EMPTY;                  /* created but never written (a crash between create and write) */
    int rc = note_decode(raw, (int)got, n, seq);
    if (rc == NOTE_OK) return SL_VALID;
    return rc == NOTE_E_NEWER ? SL_NEWER : SL_BAD;
}

static void load(void)
{
    struct note a, b; u32 sa = 0, sb = 0; i64 ea = 0, eb = 0;
    int ka = read_slot(0, &a, &sa, &ea), kb = read_slot(1, &b, &sb, &eb);
    note_clear(&N); cur_slot = -1; cur_seq = 0; dirty = locked = nostore = 0;
    if (ea || eb) {                                  /* the store could not be read at all */
        nostore = 1; status = "No disk: nothing can be saved"; say("notes load storage unavailable"); return;
    }
    if (ka == SL_NEWER || kb == SL_NEWER) { locked = 1; status = "Saved note is from a newer version: not opened"; say("notes load newer version locked"); return; }
    if (ka == SL_VALID && kb == SL_VALID) {
        if ((int)(sa - sb) > 0) { ncopy(&N, &a); cur_slot = 0; cur_seq = sa; } else { ncopy(&N, &b); cur_slot = 1; cur_seq = sb; }
    } else if (ka == SL_VALID) { ncopy(&N, &a); cur_slot = 0; cur_seq = sa; }
    else if (kb == SL_VALID) { ncopy(&N, &b); cur_slot = 1; cur_seq = sb; }
    else if (ka == SL_BAD || kb == SL_BAD) { locked = 1; status = "Saved note is damaged: not opened or overwritten"; say("notes load damaged locked"); return; }
    if (cur_slot >= 0) { status = "Loaded"; sayn("notes load ok slot=", (u64)cur_slot); } else { status = "New note"; say("notes load empty"); }
}

/* Stage the older verified save in the editor. Ctrl+S is the only disk write. */
static void load_previous(void)
{
    if (dirty) { status = "Save edits before loading a prior save"; return; }
    if (locked || cur_slot < 0) { status = "No previous save available"; return; }
    struct note previous;
    u32 sequence = 0;
    i64 err;
    if (read_slot(1 - cur_slot, &previous, &sequence, &err) != SL_VALID ||
        (int)(cur_seq - sequence) <= 0) {
        status = "No previous save available";
        return;
    }
    ncopy(&N, &previous);
    dirty = 1;
    status = "Previous save loaded. Ctrl+S to keep it";
}

/* Save the buffer. Returns 1 if the saved note was read back and matches. */
static int save(void)
{
    static u8 img[NOTE_FILE_MAX], back[NOTE_FILE_MAX + 16];
    if (locked) { status = "Saving is off: the saved note is not usable"; say("notes save refused locked"); return 0; }
    int target = cur_slot < 0 ? 0 : 1 - cur_slot; u32 seq = cur_seq + 1;
    int n = note_encode(&N, seq, img);
    i64 id = lookup_own(SLOT[target]);
    if (id == EFS_ENOENT) id = fs_create(SLOT[target], 3);
    if (id < 0) { status = fs_why(id); sayn("notes save failed create/lookup rc=", (u64)(-id)); return 0; }
    i64 w = fs_write(id, 0, img, (u64)n);
    if (w != n) { status = w < 0 ? fs_why(w) : "Short write: not saved"; sayn("notes save failed write rc=", (u64)(w < 0 ? -w : 1)); if (w == AE_FS(-17)) status = "Could not confirm the save. Reopen Notes to check"; return 0; }
    i64 got = fs_read(id, 0, back, sizeof back);                       /* read-back through the filesystem, then decode and compare */
    struct note chk; u32 cseq = 0;
    if (got < n || note_decode(back, (int)got, &chk, &cseq) != NOTE_OK || cseq != seq || chk.len != N.len) { status = "Could not verify the save"; say("notes save failed verify"); return 0; }
    for (int i = 0; i < N.len; i++) if (chk.buf[i] != N.buf[i]) { status = "Could not verify the save"; say("notes save failed verify"); return 0; }
    cur_slot = target; cur_seq = seq; dirty = 0; status = "Saved"; nostore = 0;
    sayn("notes save ok seq=", seq); return 1;
}

/* ---------------------------------------------------------------- display */
static void show_text(void)                          /* one line the tests can read: the whole note with the cursor as '|' and rows joined by '/' */
{
    char line[NOTE_MAX + 64]; int n = 0; const char *p = "notes text=";
    while (p[n]) { line[n] = p[n]; n++; }
    for (int i = 0; i <= N.len && n + 3 < (int)sizeof line; i++) {
        if (i == N.cur) line[n++] = '|';
        if (i < N.len) line[n++] = N.buf[i] == '\n' ? '/' : N.buf[i];
    }
    line[n] = 0; say(line);
}
static void refresh(void)
{
    struct nlayout l; note_layout(N.buf, N.len, &l);
    int cr, cc; note_cursor_pos(&N, &cr, &cc);
    char row[NOTE_COLS + 4]; char st[48]; int k = 0;
    const char *msg = *status ? status : (dirty ? "Unsaved changes" : "Ready");          /* an error from a failed save stays visible until the next edit */
    for (const char *s = msg; *s && k < 40; s++) st[k++] = *s;
    st[k] = 0;
    send_text(1, 0, st);
    for (int r = 0; r < NOTE_ROWS; r++) {
        int m = 0;
        if (r < l.rows) {
            for (int i = 0; i < l.len[r]; i++) row[m++] = N.buf[l.start[r] + i];
            if (r == cr && !locked) { for (int i = m; i > cc; i--) row[i] = row[i - 1]; row[cc] = '|'; m++; }   /* the cursor, drawn as '|' */
        }
        if (!m) row[m++] = ' ';
        row[m] = 0; send_text((u8)(2 + r), 1, row);
    }
    commit(); show_text();
    { char l2[64]; int n = 0; const char *p = "notes status="; while (p[n]) { l2[n] = p[n]; n++; } for (const char *s = st; *s && n + 1 < (int)sizeof l2; s++) l2[n++] = *s; l2[n] = 0; say(l2); }
}

static void edited(void) { dirty = 1; status = ""; }

static int on_key(u32 code, u32 mods)                 /* returns 1 if the screen should be redrawn, 2 to quit */
{
    if (mods & 2u) {                                   /* Ctrl commands */
        if (code == 31) { save(); return 1; }          /* Ctrl+S */
        if (code == 19) { load_previous(); return 1; } /* Ctrl+R */
        if (code == 16) return 2;                      /* Ctrl+Q */
        return 0;
    }
    if (locked) return 0;                              /* nothing to edit: the saved note was not opened */
    int c = note_key_char(code, mods);
    if (c) { if (note_insert(&N, c)) { edited(); } else { status = "Note is full"; } return 1; }
    switch (code) {
    case 14: if (note_backspace(&N)) edited(); return 1;
    case 111: if (note_delete(&N)) edited(); return 1;
    case 105: note_left(&N); return 1; case 106: note_right(&N); return 1;
    case 103: note_updown(&N, -1); return 1; case 108: note_updown(&N, 1); return 1;
    case 102: note_home(&N); return 1; case 107: note_end(&N); return 1;
    default: return 0;
    }
}

void __attribute__((section(".text._start"))) _start(u64 mode)
{
    (void)mode;
    u8 b[48], r[16]; u64 n = slen("Notes");
    hdr(b, 1, ++useq, 0); b[8] = 2; b[9] = (u8)n; for (u64 i = 0; i < n; i++) b[10 + i] = (u8)"Notes"[i];     /* size class 2: large */
    i64 rc = ui_send(b, 10 + n, r, sizeof r);
    if (rc != 0) { sayn("notes create failed=", (u64)(-rc)); quit(2); }
    handle = get32(r + 4);
    send_rect(0, 0, 8, 8, 700, 24); send_text(0, 1, "Notes");
    send_rect(1, 2, 8, 36, 700, 20);
    for (int i = 0; i < NOTE_ROWS; i++) send_rect((u8)(2 + i), 1, 8, 62 + i * 30, 700, 28);
    send_rect(10, 2, 8, 316, 700, 20); send_text(10, 0, "Ctrl+S save  Ctrl+R previous  Ctrl+Q quit");
    say("notes started");
    load(); refresh();
    for (;;) {
        u8 ev[16]; i64 got = ui_poll(ev, sizeof ev);
        if (got <= 0) { yield(); continue; }
        if (ev[1] == 0x84) { say("notes close requested"); quit(0); }
        if (ev[1] == 0x82 && ev[12]) {                                             /* a key press (not a release) */
            u32 code = (u32)ev[8] | ((u32)ev[9] << 8), mods = (u32)ev[10] | ((u32)ev[11] << 8);
            int a = on_key(code, mods);
            if (a == 2) { say(dirty ? "notes exit unsaved" : "notes exit"); quit(0); }
            if (a) refresh();
        }
    }
}
