/* tests/kernel-host/notes_test.c - the Notes core (tests/programs/notes_core.h, the same header the EL0 app compiles): editing bounds, wrapping, cursor movement, key mapping,
 * the on-disk format (round trip, every truncation, every single-byte corruption, versions, stale tails) and a randomized run against an independent oracle. Host, ASan/UBSan. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../programs/notes_core.h"

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static struct note N;
static void typ(const char *s) { for (; *s; s++) note_insert(&N, (unsigned char)*s); }
static int is(const char *s) { return (int)strlen(s) == N.len && memcmp(N.buf, s, (size_t)N.len) == 0; }

int main(void)
{
    printf("notes tests\n");
    note_clear(&N);
    CHECK("empty note", N.len == 0 && N.cur == 0);
    typ("AuroraOS is my independent operating system."); CHECK("the demo sentence is stored exactly", is("AuroraOS is my independent operating system.") && N.cur == N.len);
    CHECK("backspace at the start does nothing", (note_clear(&N), !note_backspace(&N)) && N.len == 0);
    CHECK("delete at the end does nothing", !note_delete(&N));
    CHECK("left/right at the bounds do nothing", !note_left(&N) && !note_right(&N));
    CHECK("non-printable characters are refused", !note_insert(&N, 0) && !note_insert(&N, 7) && !note_insert(&N, 0x7F) && !note_insert(&N, 0x80) && !note_insert(&N, 0xE9) && !note_insert(&N, '\r') && !note_insert(&N, '\t') && N.len == 0);
    typ("abc"); note_left(&N); note_left(&N); note_insert(&N, 'X'); CHECK("insert in the middle", is("aXbc") && N.cur == 2);
    note_backspace(&N); CHECK("backspace removes the char before the cursor", is("abc") && N.cur == 1);
    note_delete(&N); CHECK("delete removes the char at the cursor", is("ac") && N.cur == 1);
    note_home(&N); CHECK("home", N.cur == 0); note_end(&N); CHECK("end", N.cur == 2);

    { /* wrapping and rows */
        struct nlayout l; note_clear(&N);
        for (int i = 0; i < NOTE_COLS; i++) note_insert(&N, 'a');
        note_layout(N.buf, N.len, &l); CHECK("a full row stays one row (the cursor sits at its end)", l.rows == 1 && l.len[0] == NOTE_COLS);
        note_insert(&N, 'b'); note_layout(N.buf, N.len, &l); CHECK("one more character wraps to a second row", l.rows == 2 && l.len[1] == 1 && l.start[1] == NOTE_COLS);
        note_clear(&N); typ("ab"); note_insert(&N, '\n'); note_layout(N.buf, N.len, &l); CHECK("a trailing newline gives an empty last row for the cursor", l.rows == 2 && l.len[1] == 0);
        note_clear(&N); for (int i = 0; i < NOTE_ROWS - 1; i++) note_insert(&N, '\n');
        CHECK("seven newlines fill eight rows", N.len == NOTE_ROWS - 1 && note_fits(N.buf, N.len));
        CHECK("the ninth row is refused, buffer unchanged", !note_insert(&N, '\n') && N.len == NOTE_ROWS - 1);
        CHECK("a character on the last row is still accepted", note_insert(&N, 'z'));
        note_clear(&N); int ins = 0; for (int i = 0; i < 1000; i++) ins += note_insert(&N, 'q');
        CHECK("maximum length: 8 rows x 36 columns = 288 = NOTE_MAX bytes, then refused", ins == NOTE_MAX && N.len == NOTE_MAX && NOTE_ROWS * NOTE_COLS == NOTE_MAX);
        CHECK("a full note refuses every further insert", !note_insert(&N, 'x') && !note_insert(&N, '\n'));
        CHECK("delete and backspace still work on a full note", note_backspace(&N) && note_insert(&N, 'x'));
    }
    { /* cursor movement across rows */
        note_clear(&N); typ("hello"); note_insert(&N, '\n'); typ("hi"); note_insert(&N, '\n'); typ("world!");
        CHECK("cursor at the end of the last row", N.cur == N.len);
        note_updown(&N, -1); CHECK("up moves to the same column clamped to the shorter row", N.cur == 6 + 2 + 0 + 0 && N.buf[N.cur - 1] == 'i');
        note_updown(&N, -1); CHECK("up again lands on the first row", N.cur == 2);
        CHECK("up on the first row does nothing", !note_updown(&N, -1) && N.cur == 2);
        note_updown(&N, 1); note_updown(&N, 1); CHECK("down twice reaches the last row", N.cur > 9);
        CHECK("down on the last row does nothing", !note_updown(&N, 1));
        note_home(&N); CHECK("home goes to the start of the cursor's row", N.buf[N.cur - 1] == '\n' && N.cur == 9);
        note_end(&N); CHECK("end goes to the end of the row", N.cur == N.len);
    }
    { /* key mapping */
        CHECK("letters", note_key_char(30, 0) == 'a' && note_key_char(16, 0) == 'q' && note_key_char(50, 0) == 'm' && note_key_char(44, 0) == 'z' && note_key_char(25, 0) == 'p' && note_key_char(38, 0) == 'l');
        CHECK("shifted letters are upper case", note_key_char(30, 1) == 'A' && note_key_char(44, 1) == 'Z');
        CHECK("digits and shifted digits", note_key_char(2, 0) == '1' && note_key_char(11, 0) == '0' && note_key_char(2, 1) == '!' && note_key_char(11, 1) == ')');
        CHECK("space, period, comma, slash", note_key_char(57, 0) == ' ' && note_key_char(52, 0) == '.' && note_key_char(51, 0) == ',' && note_key_char(53, 0) == '/' && note_key_char(53, 1) == '?');
        CHECK("Enter and keypad Enter are newline", note_key_char(28, 0) == '\n' && note_key_char(96, 0) == '\n');
        CHECK("Ctrl and Alt combinations are never text", note_key_char(31, 2) == 0 && note_key_char(31, 4) == 0 && note_key_char(30, 2 | 1) == 0);
        CHECK("navigation, function and unknown codes give no character", note_key_char(14, 0) == 0 && note_key_char(105, 0) == 0 && note_key_char(1, 0) == 0 && note_key_char(59, 0) == 0 && note_key_char(0xFFFF, 0) == 0);
        int bad = 0; for (unsigned c = 0; c < 70000; c++) for (unsigned m = 0; m < 8; m++) { int ch = note_key_char(c, m); if (ch && !nc_printable(ch) && ch != '\n') bad = 1; }
        CHECK("no code and modifier combination ever yields a non-printable character", !bad);
    }
    { /* the file format */
        static nc_u8 f[NOTE_FILE_MAX + 8]; struct note a, b; nc_u32 seq;
        note_clear(&a); const char *t = "AuroraOS is my independent operating system."; for (const char *s = t; *s; s++) note_insert(&a, *s);
        int n = note_encode(&a, 7, f);
        CHECK("encoded length is header + text", n == NOTE_HDR + (int)strlen(t));
        CHECK("round trip", note_decode(f, n, &b, &seq) == NOTE_OK && seq == 7 && b.len == a.len && memcmp(b.buf, a.buf, (size_t)a.len) == 0 && b.cur == b.len);
        note_clear(&a); n = note_encode(&a, 1, f); CHECK("an empty note round-trips", note_decode(f, n, &b, &seq) == NOTE_OK && b.len == 0);
        note_clear(&a); for (int i = 0; i < NOTE_MAX; i++) note_insert(&a, 'k'); n = note_encode(&a, 0xFFFFFFFFu, f);
        CHECK("a maximum note round-trips (and the maximum sequence)", n == NOTE_FILE_MAX && note_decode(f, n, &b, &seq) == NOTE_OK && b.len == NOTE_MAX && seq == 0xFFFFFFFFu);
        int ok = 1; for (int k = 0; k < n; k++) if (note_decode(f, k, &b, &seq) == NOTE_OK) ok = 0;
        CHECK("EVERY truncation is refused (a torn write never loads)", ok);
        { /* a shorter rewrite over a longer file leaves a stale tail: the length field governs */
            struct note s2; note_clear(&s2); typ("x"); note_clear(&s2); note_insert(&s2, 'h'); note_insert(&s2, 'i');
            static nc_u8 g[NOTE_FILE_MAX]; memcpy(g, f, sizeof g); int m = note_encode(&s2, 2, g);
            CHECK("a shorter note over a longer old file decodes to the shorter text (stale tail ignored)", m < NOTE_FILE_MAX && note_decode(g, NOTE_FILE_MAX, &b, &seq) == NOTE_OK && b.len == 2 && seq == 2);
        }
        note_clear(&a); typ(""); { struct note s3; note_clear(&s3); for (const char *s = "data"; *s; s++) note_insert(&s3, *s); n = note_encode(&s3, 5, f); }
        int detected = 0, total = 0;
        for (int i = 0; i < n; i++) for (int bit = 0; bit < 8; bit++) { f[i] ^= (nc_u8)(1u << bit); total++; if (note_decode(f, n, &b, &seq) != NOTE_OK) detected++; f[i] ^= (nc_u8)(1u << bit); }
        CHECK("every single-bit corruption of the file is detected", detected == total);
        f[4] = 2; int rc = note_decode(f, n, &b, &seq); CHECK("a newer version is classified NEWER (never overwritten)", rc == NOTE_E_NEWER && b.len == 0); f[4] = 1;
        f[4] = 0; CHECK("version 0 is refused", note_decode(f, n, &b, &seq) == NOTE_E_VERSION); f[4] = 1;
        f[0] = 'X'; CHECK("bad magic", note_decode(f, n, &b, &seq) == NOTE_E_MAGIC); f[0] = 'A';
        f[5] = 1; CHECK("reserved flags must be zero", note_decode(f, n, &b, &seq) == NOTE_E_FLAGS); f[5] = 0;
        nc_put16(f + 6, NOTE_MAX + 1); CHECK("an oversized declared length is refused", note_decode(f, n, &b, &seq) == NOTE_E_LEN); nc_put16(f + 6, 4);
        CHECK("the restored file decodes again", note_decode(f, n, &b, &seq) == NOTE_OK);
        { /* well-formed CRC over forbidden content */
            static nc_u8 h[NOTE_FILE_MAX]; memcpy(h, f, (size_t)n); h[NOTE_HDR] = 0x01;
            nc_u32 c = nc_crc32(h + 4, 8, 0); c = nc_crc32(h + NOTE_HDR, 4, c); nc_put32(h + 12, c);
            CHECK("a control character with a valid CRC is still refused", note_decode(h, n, &b, &seq) == NOTE_E_CHARS && b.len == 0);
            memcpy(h, f, (size_t)n); for (int i = 0; i < 4; i++) h[NOTE_HDR + i] = '\n';
            nc_put16(h + 6, 4); c = nc_crc32(h + 4, 8, 0); c = nc_crc32(h + NOTE_HDR, 4, c); nc_put32(h + 12, c);
            CHECK("valid CRC and characters, within the row limit: accepted", note_decode(h, n, &b, &seq) == NOTE_OK);
            static nc_u8 big[NOTE_FILE_MAX]; memcpy(big, f, (size_t)n); nc_put16(big + 6, 9); for (int i = 0; i < 9; i++) big[NOTE_HDR + i] = '\n';
            c = nc_crc32(big + 4, 8, 0); c = nc_crc32(big + NOTE_HDR, 9, c); nc_put32(big + 12, c);
            CHECK("a file that would need more than 8 rows is refused", note_decode(big, NOTE_HDR + 9, &b, &seq) == NOTE_E_LEN);
        }
        { /* random garbage never crashes and never yields a note unless it is a valid file */
            unsigned s = 1; int accepted = 0;
            for (int it = 0; it < 200000; it++) {
                static nc_u8 r[NOTE_FILE_MAX + 16]; s = s * 1664525u + 1013904223u; int len = (int)((s >> 8) % (NOTE_FILE_MAX + 16));
                for (int i = 0; i < len; i++) { s = s * 1664525u + 1013904223u; r[i] = (nc_u8)(s >> 16); }
                if (len > 16 && (s & 1)) { r[0] = 'A'; r[1] = 'N'; r[2] = 'T'; r[3] = 'E'; r[4] = 1; r[5] = 0; }
                if (note_decode(r, len, &b, &seq) == NOTE_OK) accepted++;
            }
            CHECK("200,000 random byte strings: none accepted (a CRC must match)", accepted == 0);
        }
    }
    { /* randomized editing against a simple oracle (a std::string-like array) */
        unsigned s = 7; int bad = 0;
        for (int it = 0; it < 3000 && !bad; it++) {
            struct note n; note_clear(&n); char o[NOTE_MAX + 1]; int ol = 0, oc = 0;
            for (int k = 0; k < 200; k++) {
                s = s * 1664525u + 1013904223u; unsigned r = (s >> 8) % 12; int ch = r < 7 ? "abc de\n"[(s >> 3) % 7] : 0;
                if (r < 7) {
                    struct nlayout l; char t[NOTE_MAX + 1]; for (int i = 0; i < oc; i++) t[i] = o[i]; t[oc] = (char)ch; for (int i = oc; i < ol; i++) t[i + 1] = o[i];
                    int want = ol < NOTE_MAX && note_layout(t, ol + 1, &l) <= NOTE_ROWS;
                    int got = note_insert(&n, ch);
                    if (want != got) bad = 1;
                    if (want) { for (int i = 0; i <= ol; i++) o[i] = t[i]; ol++; oc++; }
                } else if (r == 7) { int g = note_backspace(&n); if (g != (oc > 0)) bad = 2; if (oc > 0) { for (int i = oc - 1; i + 1 < ol; i++) o[i] = o[i + 1]; ol--; oc--; } }
                else if (r == 8) { int g = note_delete(&n); if (g != (oc < ol)) bad = 3; if (oc < ol) { for (int i = oc; i + 1 < ol; i++) o[i] = o[i + 1]; ol--; } }
                else if (r == 9) { if (note_left(&n)) oc--; }
                else if (r == 10) { if (note_right(&n)) oc++; }
                else { note_updown(&n, (s >> 5) & 1 ? 1 : -1); oc = n.cur; }
                if (n.len != ol || n.cur != oc || memcmp(n.buf, o, (size_t)ol)) bad = 4;
                if (n.cur < 0 || n.cur > n.len || n.len > NOTE_MAX || !note_fits(n.buf, n.len)) bad = 5;
            }
        }
        if (bad) printf("    random edit run diverged: %d\n", bad);
        CHECK("3,000 random edit sessions (600,000 operations) match the oracle and never break a bound", bad == 0);
    }
    printf("notes: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
