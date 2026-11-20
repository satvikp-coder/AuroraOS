/*
 * tests/programs/notes_core.h - the Notes application's pure core: a bounded text buffer with a cursor, wrapped layout, key mapping and the on-disk format.
 * No libc, no system call, no allocation; the SAME header is compiled into the EL0 program (notes.c) and into the host tests (tests/kernel-host/notes_test.c).
 *
 * Editing model (exactly what is implemented, nothing more): printable ASCII (0x20..0x7E) and newline, one cursor, Left/Right/Up/Down/Home/End, Backspace, Delete.
 * No selection, clipboard/paste, Unicode, input method or undo. The note holds at most NOTE_MAX bytes AND at most NOTE_ROWS wrapped rows of NOTE_COLS columns;
 * an insertion that would exceed either bound is refused (the buffer is unchanged), never truncated silently.
 *
 * On-disk format (little endian), one file per slot, two slots ("note.a", "note.b") written alternately so a failed or torn write never destroys the last good note:
 *   0..3  magic "ANTE"   4 version u8 (=1)   5 flags u8 (=0, reserved)   6..7 length u16 (<= NOTE_MAX)   8..11 sequence u32   12..15 CRC-32 of bytes 4..11 and the body
 *   16..  body (length bytes: printable ASCII and '\n')
 * Bytes after 16+length are IGNORED (a shorter rewrite cannot shrink the file, so a stale tail may exist; the length field governs).
 * Decode result classes: OK; SHORT (cannot hold the header or its declared body: a truncated write); MAGIC; NEWER (a version above this program's: never overwritten);
 * VERSION (0), LEN, FLAGS, CRC, CHARS (a corrupt or foreign file).
 */
#ifndef NOTES_CORE_H
#define NOTES_CORE_H

typedef unsigned char nc_u8; typedef unsigned short nc_u16; typedef unsigned int nc_u32;

#define NOTE_MAX 288
#define NOTE_COLS 36
#define NOTE_ROWS 8
#define NOTE_HDR 16
#define NOTE_VERSION 1
#define NOTE_FILE_MAX (NOTE_HDR + NOTE_MAX)

enum { NOTE_OK = 0, NOTE_E_SHORT = -1, NOTE_E_MAGIC = -2, NOTE_E_NEWER = -3, NOTE_E_VERSION = -4, NOTE_E_LEN = -5, NOTE_E_FLAGS = -6, NOTE_E_CRC = -7, NOTE_E_CHARS = -8 };

struct note { char buf[NOTE_MAX]; int len, cur; };

/* ---- helpers ---- */
static nc_u32 nc_crc32(const nc_u8 *p, int n, nc_u32 crc)          /* IEEE, bitwise; crc is the running value (0 to start) */
{
    crc = ~crc;
    for (int i = 0; i < n; i++) { crc ^= p[i]; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (nc_u32)-(int)(crc & 1)); }
    return ~crc;
}
static int nc_printable(int c) { return c >= 0x20 && c <= 0x7E; }

/* ---- layout: wrapped rows. A row starts at buf index start[r] and holds len[r] characters (not counting a terminating '\n'). ---- */
struct nlayout { int rows; int start[NOTE_ROWS + 1], len[NOTE_ROWS + 1]; };
/* Fills lay (up to NOTE_ROWS+1 rows, so overflow is detectable); returns the row count. A row breaks at a '\n' (which belongs to the row it ends) or after NOTE_COLS characters. */
static int note_layout(const char *b, int n, struct nlayout *lay)
{
    int r = 0, s = 0, i = 0;
    lay->rows = 0;
    for (;;) {
        int len = 0;
        while (i < n && b[i] != '\n' && len < NOTE_COLS) { i++; len++; }
        if (r > NOTE_ROWS) break;
        lay->start[r] = s; lay->len[r] = len; r++;
        if (i < n && b[i] == '\n') i++;                                   /* the newline ends this row */
        s = i;
        if (i >= n) {                                                     /* end of text: a trailing '\n' or a completely full row leaves an empty last row for the cursor */
            if ((n > 0 && b[n - 1] == '\n') ) { if (r <= NOTE_ROWS) { lay->start[r] = s; lay->len[r] = 0; r++; } }
            break;
        }
    }
    lay->rows = r; return r;
}

static void note_clear(struct note *n) { n->len = 0; n->cur = 0; }

/* Would the text have too many rows? */
static int note_fits(const char *b, int n) { struct nlayout l; return note_layout(b, n, &l) <= NOTE_ROWS; }

/* Insert one character at the cursor. Returns 1 if inserted, 0 if refused (not printable/newline, buffer full, or too many rows). */
static int note_insert(struct note *n, int c)
{
    if (!(nc_printable(c) || c == '\n') || n->len >= NOTE_MAX || n->cur < 0 || n->cur > n->len) return 0;
    char t[NOTE_MAX + 1];
    for (int i = 0; i < n->cur; i++) t[i] = n->buf[i];
    t[n->cur] = (char)c;
    for (int i = n->cur; i < n->len; i++) t[i + 1] = n->buf[i];
    if (!note_fits(t, n->len + 1)) return 0;
    for (int i = 0; i <= n->len; i++) n->buf[i] = t[i];
    n->len++; n->cur++; return 1;
}
static int note_backspace(struct note *n)
{
    if (n->cur <= 0) return 0;
    for (int i = n->cur - 1; i + 1 < n->len; i++) n->buf[i] = n->buf[i + 1];
    n->len--; n->cur--; return 1;
}
static int note_delete(struct note *n)
{
    if (n->cur >= n->len) return 0;
    for (int i = n->cur; i + 1 < n->len; i++) n->buf[i] = n->buf[i + 1];
    n->len--; return 1;
}

/* cursor row/column in the current layout (the cursor sits AFTER the characters before it) */
static void note_cursor_pos(const struct note *n, int *row, int *col)
{
    struct nlayout l; note_layout(n->buf, n->len, &l);
    int r = 0;
    for (int k = 0; k < l.rows; k++) {
        int end = l.start[k] + l.len[k];
        if (n->cur >= l.start[k] && n->cur <= end) {
            /* a cursor exactly at a wrap point (no newline between) belongs to the NEXT row's start; a cursor before a '\n' stays at the end of its row */
            int wrap = (n->cur == end && k + 1 < l.rows && end < n->len && n->buf[end] != '\n' && l.start[k + 1] == end);
            if (!wrap) { r = k; *row = r; *col = n->cur - l.start[k]; return; }
        }
    }
    *row = l.rows ? l.rows - 1 : 0; *col = l.rows ? l.len[l.rows - 1] : 0;
}
static int note_left(struct note *n) { if (n->cur > 0) { n->cur--; return 1; } return 0; }
static int note_right(struct note *n) { if (n->cur < n->len) { n->cur++; return 1; } return 0; }
static int note_home(struct note *n) { int r, c; note_cursor_pos(n, &r, &c); if (!c) return 0; n->cur -= c; return 1; }
static int note_end(struct note *n)
{
    struct nlayout l; int r, c; note_cursor_pos(n, &r, &c); note_layout(n->buf, n->len, &l);
    int to = l.start[r] + l.len[r]; if (to == n->cur) return 0; n->cur = to; return 1;
}
static int note_updown(struct note *n, int dir)
{
    struct nlayout l; int r, c; note_cursor_pos(n, &r, &c); note_layout(n->buf, n->len, &l);
    int t = r + dir; if (t < 0 || t >= l.rows) return 0;
    if (c > l.len[t]) c = l.len[t];
    n->cur = l.start[t] + c; return 1;
}

/* ---- key mapping: evdev code + modifiers -> printable character (0 if none). Letters honour Shift only (no caps lock). ---- */
static int note_key_char(unsigned code, unsigned mods)
{
    static const char LET[] = "qwertyuiop";          /* codes 16..25 */
    static const char LET2[] = "asdfghjkl";          /* 30..38 */
    static const char LET3[] = "zxcvbnm";            /* 44..50 */
    static const char NUM[] = "1234567890", NUMS[] = "!@#$%^&*()";   /* 2..11 */
    int sh = (mods & 1u) != 0, c = 0;
    if (mods & (2u | 4u)) return 0;                  /* Ctrl or Alt combinations are commands, never text */
    if (code >= 16 && code <= 25) c = LET[code - 16];
    else if (code >= 30 && code <= 38) c = LET2[code - 30];
    else if (code >= 44 && code <= 50) c = LET3[code - 44];
    if (c) return sh ? c - 32 : c;
    if (code >= 2 && code <= 11) return sh ? NUMS[code - 2] : NUM[code - 2];
    switch (code) {
    case 12: return sh ? '_' : '-';  case 13: return sh ? '+' : '=';  case 26: return sh ? '{' : '[';  case 27: return sh ? '}' : ']';
    case 39: return sh ? ':' : ';';  case 40: return sh ? '"' : '\''; case 41: return sh ? '~' : '`';  case 43: return sh ? '|' : '\\';
    case 51: return sh ? '<' : ',';  case 52: return sh ? '>' : '.';  case 53: return sh ? '?' : '/';  case 57: return ' ';
    case 28: case 96: return '\n';
    default: return 0;
    }
}

/* ---- the on-disk format ---- */
static void nc_put16(nc_u8 *p, nc_u32 v) { p[0] = (nc_u8)v; p[1] = (nc_u8)(v >> 8); }
static void nc_put32(nc_u8 *p, nc_u32 v) { p[0] = (nc_u8)v; p[1] = (nc_u8)(v >> 8); p[2] = (nc_u8)(v >> 16); p[3] = (nc_u8)(v >> 24); }
static nc_u32 nc_get16(const nc_u8 *p) { return (nc_u32)p[0] | ((nc_u32)p[1] << 8); }
static nc_u32 nc_get32(const nc_u8 *p) { return (nc_u32)p[0] | ((nc_u32)p[1] << 8) | ((nc_u32)p[2] << 16) | ((nc_u32)p[3] << 24); }

/* Encode into out (capacity NOTE_FILE_MAX); returns the file length (NOTE_HDR + len). */
static int note_encode(const struct note *n, nc_u32 seq, nc_u8 *out)
{
    out[0] = 'A'; out[1] = 'N'; out[2] = 'T'; out[3] = 'E'; out[4] = NOTE_VERSION; out[5] = 0;
    nc_put16(out + 6, (nc_u32)n->len); nc_put32(out + 8, seq);
    for (int i = 0; i < n->len; i++) out[NOTE_HDR + i] = (nc_u8)n->buf[i];
    nc_u32 c = nc_crc32(out + 4, 8, 0); c = nc_crc32(out + NOTE_HDR, n->len, c);
    nc_put32(out + 12, c);
    return NOTE_HDR + n->len;
}
/* Decode untrusted bytes. On NOTE_OK, *n holds the text (cursor at the end) and *seq the sequence number; otherwise n is cleared. */
static int note_decode(const nc_u8 *in, int len, struct note *n, nc_u32 *seq)
{
    note_clear(n); *seq = 0;
    if (len < NOTE_HDR) return NOTE_E_SHORT;
    if (in[0] != 'A' || in[1] != 'N' || in[2] != 'T' || in[3] != 'E') return NOTE_E_MAGIC;
    if (in[4] > NOTE_VERSION) return NOTE_E_NEWER;
    if (in[4] != NOTE_VERSION) return NOTE_E_VERSION;
    if (in[5] != 0) return NOTE_E_FLAGS;
    int bl = (int)nc_get16(in + 6);
    if (bl > NOTE_MAX) return NOTE_E_LEN;
    if (len < NOTE_HDR + bl) return NOTE_E_SHORT;
    nc_u32 c = nc_crc32(in + 4, 8, 0); c = nc_crc32(in + NOTE_HDR, bl, c);
    if (c != nc_get32(in + 12)) return NOTE_E_CRC;
    for (int i = 0; i < bl; i++) if (!(nc_printable(in[NOTE_HDR + i]) || in[NOTE_HDR + i] == '\n')) return NOTE_E_CHARS;
    if (!note_fits((const char *)in + NOTE_HDR, bl)) return NOTE_E_LEN;
    for (int i = 0; i < bl; i++) n->buf[i] = (char)in[NOTE_HDR + i];
    n->len = bl; n->cur = bl; *seq = nc_get32(in + 8);
    return NOTE_OK;
}
#endif
