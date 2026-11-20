/* tests/kernel-host/files_test.c - the Files viewer core (tests/programs/files_core.h, the header the EL0 app compiles): selection and scrolling bounds, row formatting of hostile names
 * and sizes, and the bounded text preview (printable-only, binary refused, truncation marked). Host, ASan/UBSan. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../programs/files_core.h"

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static struct fent mk(const char *name, fc_u32 size, fc_u32 owner) { struct fent e; memset(&e, 0, sizeof e); strncpy(e.name, name, 31); e.size = size; e.owner = owner; e.id = 1; return e; }

int main(void)
{
    printf("files tests\n");
    struct fview v;
    fv_init(&v, 0); CHECK("an empty list cannot move", !fv_move(&v, 1) && !fv_move(&v, -1) && v.sel == 0 && v.top == 0);
    fv_init(&v, 100); CHECK("a list never exceeds the 16 entries the kernel returns", v.n == F_MAX);
    fv_init(&v, -5); CHECK("a negative count is an empty list", v.n == 0);
    fv_init(&v, 3); CHECK("up at the top does nothing", !fv_move(&v, -1) && v.sel == 0);
    CHECK("down moves", fv_move(&v, 1) && v.sel == 1); fv_move(&v, 1); CHECK("down at the bottom does nothing", !fv_move(&v, 1) && v.sel == 2 && v.top == 0);
    fv_init(&v, 16);
    int ok = 1; for (int i = 0; i < 15; i++) { fv_move(&v, 1); if (v.sel < v.top || v.sel >= v.top + F_ROWS) ok = 0; }
    CHECK("scrolling down keeps the selection inside the visible window", ok && v.sel == 15 && v.top == 8);
    for (int i = 0; i < 15; i++) { fv_move(&v, -1); if (v.sel < v.top || v.sel >= v.top + F_ROWS) ok = 0; }
    CHECK("scrolling back up keeps it inside too", ok && v.sel == 0 && v.top == 0);
    CHECK("a huge step is clamped", (fv_move(&v, 1000), v.sel == 15) && (fv_move(&v, -1000), v.sel == 0));
    { unsigned s = 5; int bad = 0; fv_init(&v, 16);
      for (int i = 0; i < 200000; i++) { s = s * 1664525u + 1013904223u; fv_move(&v, (int)((s >> 10) % 5) - 2); if (v.sel < 0 || v.sel >= v.n || v.top < 0 || v.sel < v.top || v.sel >= v.top + F_ROWS || v.top + F_ROWS > v.n + F_ROWS) bad = 1; }
      CHECK("200,000 random moves never leave the bounds", !bad); }

    char row[48];
    struct fent e = mk("readme.txt", 1234, 700);
    fc_row(row, sizeof row, &e, 1, 603); CHECK("a selected row", strcmp(row, "> readme.txt 1234 B other") == 0);
    fc_row(row, sizeof row, &e, 0, 603); CHECK("an unselected row", strcmp(row, "  readme.txt 1234 B other") == 0);
    e = mk("calc.elf", 8360, 0x7F000001u); fc_row(row, sizeof row, &e, 0, 603); CHECK("installer files are 'system', never a raw principal id", strcmp(row, "  calc.elf 8360 B system") == 0);
    e = mk("mine", 0, 603); fc_row(row, sizeof row, &e, 0, 603); CHECK("own files say 'you' and an empty file has size 0", strcmp(row, "  mine 0 B you") == 0);
    e = mk("abcdefghijklmnopqrstuvwxyz01234", 4294967295u, 5); fc_row(row, sizeof row, &e, 1, 603);
    CHECK("a 31-character name is cut with '...'; an absurd claimed size is clamped; the row fits a 40-character widget", strstr(row, "abcdefghijklmnopqr...") && strstr(row, ">99999 B") && strlen(row) <= 40);
    e = mk("a\x01\x7f\x80z", 1, 5); fc_row(row, sizeof row, &e, 0, 603); CHECK("control and high bytes in a name are shown as '?'", strstr(row, "a???z") != 0);
    { struct fent h; memset(&h, 'x', sizeof h); h.size = 9; h.owner = 5; fc_row(row, sizeof row, &h, 0, 603);
      CHECK("a name field with no terminator is bounded (no overread, still <= 40 chars)", strlen(row) <= 40); }
    { int bad = 0; unsigned s = 3; for (int i = 0; i < 20000; i++) { struct fent h; for (unsigned k = 0; k < sizeof h; k++) { s = s * 1664525u + 1013904223u; ((unsigned char *)&h)[k] = (unsigned char)(s >> 16); }
          char r2[44]; fc_row(r2, sizeof r2, &h, (int)(s & 1), 603); if (strlen(r2) > 40) bad = 1; for (char *p = r2; *p; p++) if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) bad = 1; }
      CHECK("20,000 random entries always produce a printable row of at most 40 characters", !bad); }

    char L[F_PREVIEW_LINES][F_COLS + 4]; int used;
    CHECK("an empty file is EMPTY", fc_preview((const fc_u8 *)"", 0, 0, L, &used) == F_P_EMPTY && used == 0);
    CHECK("a read error count is EMPTY, not content", fc_preview((const fc_u8 *)"abc", -1, 3, L, &used) == F_P_EMPTY);
    CHECK("plain text", fc_preview((const fc_u8 *)"hello world", 11, 11, L, &used) == F_P_TEXT && used == 1 && strcmp(L[0], "hello world") == 0);
    CHECK("newlines split lines", fc_preview((const fc_u8 *)"a\nbb\nccc", 8, 8, L, &used) == F_P_TEXT && used == 3 && !strcmp(L[0], "a") && !strcmp(L[1], "bb") && !strcmp(L[2], "ccc"));
    CHECK("a NUL byte makes it binary and no byte is copied", fc_preview((const fc_u8 *)"ab\0cd", 5, 5, L, &used) == F_P_BINARY && used == 0 && L[0][0] == 0);
    CHECK("an ELF header is binary", fc_preview((const fc_u8 *)"\x7f" "ELF....", 8, 100, L, &used) == F_P_BINARY);
    CHECK("tab, CR, escape and high bytes are binary", fc_preview((const fc_u8 *)"a\tb", 3, 3, L, &used) == F_P_BINARY && fc_preview((const fc_u8 *)"a\rb", 3, 3, L, &used) == F_P_BINARY && fc_preview((const fc_u8 *)"a\x1b[0m", 5, 5, L, &used) == F_P_BINARY && fc_preview((const fc_u8 *)"caf\xc3\xa9", 5, 5, L, &used) == F_P_BINARY);
    { unsigned char b[F_PREVIEW_BYTES]; memset(b, 'x', sizeof b);
      int k = fc_preview(b, F_PREVIEW_BYTES, F_PREVIEW_BYTES, L, &used);
      CHECK("exactly 144 printable bytes fill 4 full lines with no ellipsis when that is the whole file", k == F_P_TEXT && used == 4 && strlen(L[3]) == F_COLS && strchr(L[3], '.') == 0);
      k = fc_preview(b, F_PREVIEW_BYTES, 5000, L, &used);
      CHECK("a larger file is marked with '...'", k == F_P_TEXT && used == 4 && strlen(L[3]) == F_COLS + 3 && strcmp(L[3] + F_COLS, "...") == 0);
      k = fc_preview(b, 40, 40, L, &used); CHECK("40 chars wrap at 36 columns", k == F_P_TEXT && used == 2 && strlen(L[0]) == F_COLS && strlen(L[1]) == 4); }
    { unsigned char b[F_PREVIEW_BYTES]; memset(b, '\n', sizeof b);
      int k = fc_preview(b, F_PREVIEW_BYTES, F_PREVIEW_BYTES, L, &used); CHECK("144 newlines: bounded to 4 lines, marked as cut", k == F_P_TEXT && used == 4 && strcmp(L[3], "...") == 0); }
    { int bad = 0; unsigned s = 11; for (int i = 0; i < 100000; i++) { unsigned char b[F_PREVIEW_BYTES + 1]; s = s * 1664525u + 1013904223u; int n = (int)((s >> 8) % (F_PREVIEW_BYTES + 1));
          int textual = (s & 3) != 0; for (int k = 0; k < n; k++) { s = s * 1664525u + 1013904223u; b[k] = textual ? (unsigned char)((s >> 12) % 3 == 0 ? '\n' : 0x20 + (s >> 16) % 95) : (unsigned char)(s >> 16); }
          int kind = fc_preview(b, n, (fc_u32)n + (s & 1), L, &used);
          if (kind == F_P_TEXT) { if (used < 1 || used > F_PREVIEW_LINES) bad = 1; for (int r = 0; r < F_PREVIEW_LINES; r++) { if (strlen(L[r]) > F_COLS + 3) bad = 1; for (char *p = L[r]; *p; p++) if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) bad = 1; } }
          else if (used != 0 || L[0][0]) bad = 1; }
      CHECK("100,000 random buffers: text is always printable and bounded; non-text copies nothing", !bad); }
    printf("files: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
