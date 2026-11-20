/*
 * tests/programs/files_core.h - the Files viewer's pure core: list selection/scrolling, row formatting and the bounded text preview. No libc, no system call, no allocation;
 * compiled into the EL0 program (files.c) and into the host tests (tests/kernel-host/files_test.c).
 * Everything shown is untrusted data from the filesystem: names are sanitized to printable ASCII, sizes are printed from integers, and file bytes are only ever shown as text
 * if EVERY byte of the previewed range is printable ASCII, a space or a newline (otherwise the file is reported as binary and no byte of it is displayed).
 */
#ifndef FILES_CORE_H
#define FILES_CORE_H

typedef unsigned char fc_u8; typedef unsigned int fc_u32; typedef unsigned long fc_u64;

#define F_ROWS 8                 /* visible list rows */
#define F_MAX 16                 /* SYS_FS LIST returns at most 16 entries (SYS_FS_LIST_MAX) */
#define F_NAME 31
#define F_COLS 36                /* preview columns */
#define F_PREVIEW_LINES 4
#define F_PREVIEW_BYTES (F_COLS * F_PREVIEW_LINES)   /* 144 */
#define F_RESERVED_MASK 0xFF000000u

struct fent { fc_u64 id; fc_u32 size, owner, perm, reserved; char name[32]; };   /* the layout of the kernel's struct user_fs_stat */
struct fview { int n, sel, top; };

static void fv_init(struct fview *v, int n) { v->n = n < 0 ? 0 : (n > F_MAX ? F_MAX : n); v->sel = 0; v->top = 0; }
static int fv_move(struct fview *v, int d)       /* returns 1 if the selection changed */
{
    int s = v->sel + d;
    if (v->n == 0) return 0;
    if (s < 0) s = 0;
    if (s >= v->n) s = v->n - 1;
    if (s == v->sel) return 0;
    v->sel = s;
    if (v->sel < v->top) v->top = v->sel;
    if (v->sel >= v->top + F_ROWS) v->top = v->sel - F_ROWS + 1;
    return 1;
}

static int fc_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void fc_cat(char *o, int *n, int cap, const char *s) { while (*s && *n + 1 < cap) o[(*n)++] = *s++; o[*n] = 0; }
static void fc_dec(char *o, int *n, int cap, fc_u32 v) { char d[12]; int k = 0; if (!v) d[k++] = '0'; while (v) { d[k++] = (char)('0' + v % 10); v /= 10; } while (k && *n + 1 < cap) o[(*n)++] = d[--k]; o[*n] = 0; }

/* Who owns it, in words: never a raw principal number. */
static const char *fc_owner(fc_u32 owner, fc_u32 me) { return owner == me ? "you" : ((owner & F_RESERVED_MASK) == 0x7F000000u ? "system" : "other"); }

/* One list row: "> name  size B  owner" (<= 40 chars incl. NUL at 41). The name is cut to 21 characters with "..." and any non-printable byte is shown as '?'. */
static void fc_row(char *out, int cap, const struct fent *e, int selected, fc_u32 me)
{
    int n = 0; char nm[24]; int k = 0, len = 0;
    out[0] = 0;
    fc_cat(out, &n, cap, selected ? "> " : "  ");
    while (len < F_NAME && e->name[len]) len++;                 /* the name field is bounded: never trust the terminator */
    for (int i = 0; i < len && k < 21; i++) { fc_u8 c = (fc_u8)e->name[i]; nm[k++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?'; }
    if (len > 21) { k = 18; nm[k++] = '.'; nm[k++] = '.'; nm[k++] = '.'; }
    nm[k] = 0;
    fc_cat(out, &n, cap, nm); fc_cat(out, &n, cap, " ");
    if (e->size > 99999u) fc_cat(out, &n, cap, ">99999"); else fc_dec(out, &n, cap, e->size);   /* AuroraFS files are <= 24576 bytes; a larger claimed size is shown clamped, never overflowing the row */
    fc_cat(out, &n, cap, " B "); fc_cat(out, &n, cap, fc_owner(e->owner, me));
}

enum { F_P_TEXT = 0, F_P_EMPTY = 1, F_P_BINARY = 2 };
/* Classify and wrap the first bytes of a file (n bytes read, total size `size`). For F_P_TEXT fills lines[0..F_PREVIEW_LINES) (each <= F_COLS chars + NUL) and sets *used;
 * a trailing "..." marks that more of the file exists. For EMPTY and BINARY no content byte is copied. */
static int fc_preview(const fc_u8 *b, int n, fc_u32 size, char lines[F_PREVIEW_LINES][F_COLS + 4], int *used)
{
    *used = 0;
    for (int i = 0; i < F_PREVIEW_LINES; i++) lines[i][0] = 0;
    if (n <= 0) return F_P_EMPTY;
    for (int i = 0; i < n; i++) if (!((b[i] >= 0x20 && b[i] <= 0x7E) || b[i] == '\n')) return F_P_BINARY;
    int r = 0, c = 0, cut = 0;
    for (int i = 0; i < n; i++) {
        if (b[i] == '\n') { lines[r][c] = 0; if (++r >= F_PREVIEW_LINES) { r = F_PREVIEW_LINES - 1; cut = i + 1 < n; break; } c = 0; continue; }
        if (c >= F_COLS) { lines[r][c] = 0; if (++r >= F_PREVIEW_LINES) { r = F_PREVIEW_LINES - 1; c = F_COLS; cut = 1; break; } c = 0; }
        lines[r][c++] = (char)b[i];
    }
    lines[r][c] = 0; *used = r + 1;
    if (cut || (fc_u32)n < size) { int l = fc_len(lines[r]); if (l > F_COLS) l = F_COLS; lines[r][l] = '.'; lines[r][l + 1] = '.'; lines[r][l + 2] = '.'; lines[r][l + 3] = 0; }
    return F_P_TEXT;
}
#endif
