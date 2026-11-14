/* tests/kernel-host/index_model_test.c - host tests for kernel/ui/index_model.c. Vectors are ported from tests/unit/index.test.js (the
 * browser model's executable specification) plus bounds, hostile-input and determinism checks. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <index_model.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; printf("  PASS: %s\n", name); } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)
#define T(q, l) idx_match_tier((q), (l), 0, 0)

int main(void)
{
    printf("index_model host tests\n");
    char b[IDX_NORM_CAP];

    /* norm */
    CHECK("norm lowercases, trims and collapses whitespace", idx_norm("  Hello   WORLD \t x ", b, sizeof b) == 13 && strcmp(b, "hello world x") == 0);
    CHECK("norm drops control and non-ASCII bytes", (idx_norm("a\x01" "b\xc3\xa9" "c\x7f", b, sizeof b), strcmp(b, "abc") == 0));
    CHECK("norm of NULL and empty is empty", idx_norm(0, b, sizeof b) == 0 && b[0] == 0 && idx_norm("", b, sizeof b) == 0);
    { char s[300]; memset(s, 'A', 299); s[299] = 0; size_t n = idx_norm(s, b, sizeof b); CHECK("norm truncates at the buffer size and NUL-terminates", n == sizeof b - 1 && b[n] == 0 && b[0] == 'a'); }
    { char small[4]; size_t n = idx_norm("abcdef", small, sizeof small); CHECK("norm into a 4-byte buffer keeps 3 characters + NUL", n == 3 && strcmp(small, "abc") == 0); }
    { char one[1]; size_t n = idx_norm("abc", one, 1); CHECK("norm with cap 1 writes only the NUL", n == 0 && one[0] == 0 && idx_norm("abc", one, 0) == 0); }
    CHECK("norm: space after a dropped byte still collapses correctly", (idx_norm("a \x01 b", b, sizeof b), strcmp(b, "a b") == 0));

    /* tiers (ported) */
    CHECK("exact beats prefix", T("notes", "Notes") == IDX_EXACT && T("note", "Notes") == IDX_PREFIX);
    CHECK("case and extra spaces are ignored", T("  NOTES ", "notes") == IDX_EXACT);
    CHECK("word prefix: the query starts a later word", T("recipe", "Dal recipe.txt") == IDX_WORD_PREFIX && T("txt", "Dal recipe.txt") == IDX_WORD_PREFIX);
    CHECK("several typed words each start some word, in any order", T("mode dark", "Dark mode") == IDX_WORD_PREFIX && T("rec dal", "Dal recipe.txt") == IDX_WORD_PREFIX);
    CHECK("substring is weaker than word prefix", T("ecip", "Dal recipe.txt") == IDX_SUBSTRING);
    { const char *kw[] = { "multitasking" };
      CHECK("keywords come after label matches", idx_match_tier("multitasking", "Pages", kw, 1) == IDX_KEYWORD && idx_match_tier("zzz", "Pages", kw, 1) == IDX_NONE); }
    CHECK("an empty or blank query matches everything at the same tier", T("", "Anything") == IDX_PREFIX && T("   ", "x") == IDX_PREFIX && idx_match_tier(0, 0, 0, 0) == IDX_PREFIX);
    CHECK("NULL label does not crash", idx_match_tier("a", 0, 0, 0) == IDX_NONE);
    CHECK("a typed word longer than a label word is not a word-prefix hit", T("spacesx", "Spaces") == IDX_NONE && T("dark mo", "dark") == IDX_NONE);
    CHECK("a query longer than the buffer is truncated, never overflows", T("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "a") == IDX_NONE);

    /* edit distance (ported) */
    CHECK("distance basics", idx_edit_distance("clock", "clock", 5) == 0 && idx_edit_distance("clok", "clock", 5) == 1 && idx_edit_distance("clocks", "clock", 5) == 1 && idx_edit_distance("clack", "clock", 5) == 1);
    CHECK("a swap of two neighbours is one edit", idx_edit_distance("clcok", "clock", 5) == 1);
    CHECK("larger distances and early exit", idx_edit_distance("kitten", "sitting", 9) == 3 && idx_edit_distance("abcdef", "uvwxyz", 1) > 1 && idx_edit_distance("a", "abcdef", 2) > 2);
    CHECK("distance with empty strings", idx_edit_distance("", "abc", 9) == 3 && idx_edit_distance("abc", "", 9) == 3 && idx_edit_distance("", "", 9) == 0);
    { char big[100]; memset(big, 'a', 99); big[99] = 0; CHECK("over-long strings are refused safely", idx_edit_distance(big, "a", 3) > 3 && idx_edit_distance("a", big, 3) > 3); }

    /* fuzzy (ported) */
    int d;
    CHECK("a typo of an app name is found (clok -> Clock)", idx_fuzzy("clok", "Clock", &d) && d == 1);
    CHECK("short queries (< 4 characters) are never fuzzy", !idx_fuzzy("clo", "Clock", &d) && !idx_fuzzy("clk", "Clock", &d));
    CHECK("a 3-character query is never fuzzy even one edit from a 4-letter name (hme -> Home)", !idx_fuzzy("hme", "Home", &d));
    CHECK("queries with a space are never fuzzy", !idx_fuzzy("cl ock", "Clock", &d));
    CHECK("2 edits are allowed only from 8 characters", !idx_fuzzy("claok", "Clock", &d) && idx_fuzzy("calcluater", "Calculator", &d) && d == 2);
    CHECK("the first letter must agree", !idx_fuzzy("zlock", "Clock", &d));
    CHECK("anything that already matches (exact/prefix/substring) is not fuzzy", !idx_fuzzy("clock", "Clock", &d) && !idx_fuzzy("cloc", "Clock", &d));
    CHECK("an unrelated word is not fuzzy", !idx_fuzzy("zzzz", "Clock", &d));
    CHECK("fuzzy compares whole words too (sttings -> Quick Settings)", idx_fuzzy("settngs", "Quick Settings", &d));

    /* search + ordering */
    static const struct idx_item items[] = {
        { "home", "Home", { 0, 0, 0 }, 1 }, { "spaces", "Spaces", { "workspace", 0, 0 }, 1 }, { "index", "Index", { "search", "find", 0 }, 1 },
        { "study", "Study", { 0, 0, 0 }, 0 }, { "work", "Work", { 0, 0, 0 }, 0 }, { "pages", "Pages", { "multitasking", 0, 0 }, 1 },
        { "settings", "Settings", { 0, 0, 0 }, 1 }, { "about", "About AuroraOS", { 0, 0, 0 }, 0 } };
    const size_t N = sizeof items / sizeof items[0];
    struct idx_result r[IDX_MAX_RESULTS];
    size_t n = idx_search(items, N, "", 0, 0, r, IDX_MAX_RESULTS);
    int in_order = n == N; for (size_t i = 0; i < n && in_order; i++) if (r[i].item != i) in_order = 0;
    CHECK("an empty query lists everything in source order", in_order);
    n = idx_search(items, N, "s", 0, 0, r, IDX_MAX_RESULTS);
    CHECK("'s': prefix matches first (Spaces, Study, Settings in source order), then word/substring matches", n >= 3 && items[r[0].item].key[0] == 's' && r[0].tier == IDX_PREFIX && r[1].tier == IDX_PREFIX && r[2].tier == IDX_PREFIX);
    n = idx_search(items, N, "SPACES", 0, 0, r, IDX_MAX_RESULTS);
    CHECK("exact match ranks first regardless of case", n >= 1 && r[0].tier == IDX_EXACT && strcmp(items[r[0].item].key, "spaces") == 0);
    n = idx_search(items, N, "multitask", 0, 0, r, IDX_MAX_RESULTS);
    CHECK("a keyword finds an item whose label does not match", n == 1 && strcmp(items[r[0].item].key, "pages") == 0 && r[0].tier == IDX_KEYWORD);
    n = idx_search(items, N, "serach", 0, 0, r, IDX_MAX_RESULTS);
    CHECK("a typo of a KEYWORD (serach for search) finds nothing: typo suggestions use app names only", n == 0);
    n = idx_search(items, N, "settngs", 0, 0, r, IDX_MAX_RESULTS);
    CHECK("a typo of an app name yields a flagged suggestion ranked last, never the default (settngs -> Settings)", n == 1 && r[0].fuzzy == 1 && idx_default_active(r, n) == -1 && strcmp(items[r[0].item].key, "settings") == 0);
    n = idx_search(items, N, "hmoe", 0, 0, r, IDX_MAX_RESULTS);
    CHECK("'hmoe' suggests Home as fuzzy (swap), Enter would do nothing", n == 1 && r[0].fuzzy && idx_default_active(r, n) == -1);
    n = idx_search(items, N, "stdy", 0, 0, r, IDX_MAX_RESULTS);
    CHECK("typo suggestions apply only to app names (Study is not an app)", n == 0);
    n = idx_search(items, N, "zzzzqq", 0, 0, r, IDX_MAX_RESULTS);
    CHECK("no match: empty result list and no default", n == 0 && idx_default_active(r, n) == -1);
    { const char *rec[] = { "settings", "study" };
      n = idx_search(items, N, "s", rec, 2, r, IDX_MAX_RESULTS);
      CHECK("recency breaks ties inside a tier (Settings, Study before Spaces)", n >= 3 && strcmp(items[r[0].item].key, "settings") == 0 && strcmp(items[r[1].item].key, "study") == 0 && strcmp(items[r[2].item].key, "spaces") == 0);
      const char *rec2[] = { "about" };
      n = idx_search(items, N, "space", rec2, 1, r, IDX_MAX_RESULTS);
      CHECK("recency never lifts a weaker tier above a stronger one", n >= 1 && strcmp(items[r[0].item].key, "spaces") == 0); }
    { struct idx_result a[IDX_MAX_RESULTS], c[IDX_MAX_RESULTS]; size_t n1 = idx_search(items, N, "s", 0, 0, a, 16), n2 = idx_search(items, N, "s", 0, 0, c, 16);
      CHECK("the order is deterministic", n1 == n2 && memcmp(a, c, n1 * sizeof a[0]) == 0); }
    n = idx_search(items, N, "", 0, 0, r, 3);
    CHECK("results are truncated to the caller's maximum", n == 3);
    CHECK("max 0 or NULL output is safe", idx_search(items, N, "", 0, 0, r, 0) == 0 && idx_search(items, N, "", 0, 0, 0, 5) == 0 && idx_search(0, 0, "", 0, 0, r, 5) == 0);
    CHECK("defaultActive picks the first non-fuzzy result", (r[0].fuzzy = 1, r[1].fuzzy = 0, idx_default_active(r, 2) == 1));

    /* hostile input */
    { char junk[200]; for (int i = 0; i < 199; i++) junk[i] = (char)(i * 7 + 3); junk[199] = 0;
      n = idx_search(items, N, junk, 0, 0, r, 16);
      CHECK("binary garbage as a query is safe (bounded, no crash)", n <= N); }
    { char longq[600]; memset(longq, 's', 599); longq[599] = 0; n = idx_search(items, N, longq, 0, 0, r, 16); CHECK("a 599-character query is safe", n <= N); }
    { struct idx_item many[256]; for (int i = 0; i < 256; i++) many[i] = (struct idx_item){ "k", "Item", { 0, 0, 0 }, 1 };
      struct idx_result big[256]; n = idx_search(many, 300, "item", 0, 0, big, 256);
      CHECK("an oversized item count is clamped to 256, not read past", n == 256); }

    printf("index_model: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
