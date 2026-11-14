/* kernel/ui/index_model.c - native Index relevance model; see kernel/include/index_model.h. */
#include <index_model.h>

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int is_sep(char c) { return c == ' ' || c == '-' || c == '_' || c == '.'; }

size_t idx_norm(const char *s, char *out, size_t cap)
{
    if (!cap) return 0;
    size_t n = 0; int pending_space = 0;
    if (s) for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (is_ws((char)c)) { pending_space = n > 0; continue; }       /* leading whitespace is dropped, runs collapse */
        if (c < 0x21 || c > 0x7e) continue;                            /* control and non-ASCII bytes are dropped */
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (pending_space) { if (n + 1 >= cap) break; out[n++] = ' '; pending_space = 0; }
        if (n + 1 >= cap) break;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

static int starts_with(const char *s, const char *p) { while (*p) { if (*s++ != *p++) return 0; } return 1; }
static int contains(const char *s, const char *p)
{
    if (!*p) return 1;
    for (; *s; s++) if (starts_with(s, p)) return 1;
    return 0;
}

/* Split a normalised string into word start offsets (separators: space - _ .). Returns the count, at most `max`. */
static size_t words_of(const char *s, size_t *start, size_t *len, size_t max)
{
    size_t n = 0, i = 0;
    while (s[i]) {
        while (s[i] && is_sep(s[i])) i++;
        if (!s[i]) break;
        size_t b = i;
        while (s[i] && !is_sep(s[i])) i++;
        if (n < max) { start[n] = b; len[n] = i - b; n++; }
    }
    return n;
}

static int word_starts(const char *label, size_t ws, const char *q, size_t ql)
{
    for (size_t i = 0; i < ql; i++) if (label[ws + i] != q[i] || !label[ws + i]) return 0;
    return 1;
}

int idx_match_tier(const char *query, const char *label, const char *const *keywords, size_t nkw)
{
    char nq[IDX_NORM_CAP], nl[IDX_NORM_CAP];
    size_t qn = idx_norm(query, nq, sizeof nq);
    if (!qn) return IDX_PREFIX;
    idx_norm(label, nl, sizeof nl);
    size_t i = 0; while (nl[i] && nl[i] == nq[i]) i++;
    if (!nl[i] && !nq[i]) return IDX_EXACT;
    if (starts_with(nl, nq)) return IDX_PREFIX;
    size_t ls[16], ll[16], qs[16], ql[16];
    size_t lw = words_of(nl, ls, ll, 16), qw = words_of(nq, qs, ql, 16);
    for (size_t w = 0; w < lw; w++) if (qn <= ll[w] && word_starts(nl, ls[w], nq, qn)) return IDX_WORD_PREFIX;
    if (qw > 1) {
        int all = 1;
        for (size_t a = 0; a < qw && all; a++) {
            int any = 0;
            for (size_t b = 0; b < lw; b++) if (ql[a] <= ll[b] && word_starts(nl, ls[b], nq + qs[a], ql[a])) { any = 1; break; }
            if (!any) all = 0;
        }
        if (all) return IDX_WORD_PREFIX;
    }
    if (contains(nl, nq)) return IDX_SUBSTRING;
    for (size_t k = 0; k < nkw; k++) {
        if (!keywords || !keywords[k]) continue;
        char nk[IDX_NORM_CAP]; idx_norm(keywords[k], nk, sizeof nk);
        if (contains(nk, nq)) return IDX_KEYWORD;
    }
    return IDX_NONE;
}

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int idx_edit_distance(const char *a, const char *b, int max)
{
    size_t n = slen(a), m = slen(b);
    if (n >= IDX_NORM_CAP || m >= IDX_NORM_CAP) return max + 1;                 /* only ever called on normalised, bounded strings */
    int lim = max < 0 ? 0 : max;
    int diff = (int)n > (int)m ? (int)n - (int)m : (int)m - (int)n;
    if (diff > lim) return lim + 1;
    int p2[IDX_NORM_CAP + 1], p1[IDX_NORM_CAP + 1], cur[IDX_NORM_CAP + 1];
    for (size_t j = 0; j <= m; j++) { p1[j] = (int)j; p2[j] = 0; }
    for (size_t i = 1; i <= n; i++) {
        cur[0] = (int)i; int row_min = cur[0];
        for (size_t j = 1; j <= m; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int v = p1[j] + 1;
            if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
            if (p1[j - 1] + cost < v) v = p1[j - 1] + cost;
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && p2[j - 2] + 1 < v) v = p2[j - 2] + 1;
            cur[j] = v; if (v < row_min) row_min = v;
        }
        if (row_min > lim) return lim + 1;
        for (size_t j = 0; j <= m; j++) { p2[j] = p1[j]; p1[j] = cur[j]; }
    }
    return p1[m];
}

int idx_fuzzy(const char *query, const char *name, int *distance)
{
    char nq[IDX_NORM_CAP], nn[IDX_NORM_CAP];
    size_t qn = idx_norm(query, nq, sizeof nq);
    if (qn < IDX_MIN_FUZZY_LEN) return 0;
    for (size_t i = 0; i < qn; i++) if (nq[i] == ' ') return 0;
    if (idx_match_tier(nq, name, 0, 0) != IDX_NONE) return 0;
    idx_norm(name, nn, sizeof nn);
    int allow = qn >= 8 ? 2 : 1, best = allow + 1;
    char cand[IDX_NORM_CAP];
    size_t ws[16], wl[16]; size_t nw = words_of(nn, ws, wl, 16);
    for (size_t c = 0; c <= nw; c++) {
        size_t len;
        if (c == 0) { len = slen(nn); for (size_t i = 0; i <= len; i++) cand[i] = nn[i]; }
        else { len = wl[c - 1]; for (size_t i = 0; i < len; i++) cand[i] = nn[ws[c - 1] + i]; cand[len] = 0; }
        if (len < IDX_MIN_FUZZY_LEN || cand[0] != nq[0]) continue;
        int d = idx_edit_distance(nq, cand, allow);
        if (d <= allow && d < best) best = d;
    }
    if (best > allow) return 0;
    if (distance) *distance = best;
    return 1;
}

static size_t recency(const char *const *recent, size_t nrecent, const char *key)
{
    if (!key) return (size_t)-1;
    for (size_t i = 0; i < nrecent; i++) {
        const char *r = recent[i]; size_t j = 0;
        if (!r) continue;
        while (r[j] && r[j] == key[j]) j++;
        if (!r[j] && !key[j]) return i;
    }
    return (size_t)-1;
}

size_t idx_search(const struct idx_item *items, size_t n, const char *query, const char *const *recent, size_t nrecent,
                  struct idx_result *out, size_t max)
{
    if (!items || !out || !max) return 0;
    if (n > 256) n = 256;
    struct idx_result all[256]; size_t na = 0;
    for (size_t i = 0; i < n; i++) {
        int t = idx_match_tier(query, items[i].label, items[i].kw, 3);
        if (t != IDX_NONE) { all[na].item = (uint16_t)i; all[na].tier = (int8_t)t; all[na].fuzzy = 0; na++; continue; }
        int d;
        if (items[i].is_app && idx_fuzzy(query, items[i].label, &d)) { all[na].item = (uint16_t)i; all[na].tier = IDX_FUZZY; all[na].fuzzy = 1; na++; }
    }
    for (size_t i = 1; i < na; i++) {                                         /* insertion sort: stable, deterministic, no allocation */
        struct idx_result v = all[i]; size_t j = i;
        size_t rv = recency(recent, nrecent, items[v.item].key);
        while (j > 0) {
            struct idx_result u = all[j - 1];
            size_t ru = recency(recent, nrecent, items[u.item].key);
            int later = u.tier > v.tier || (u.tier == v.tier && ru > rv);      /* u must come after v */
            if (!later) break;
            all[j] = u; j--;
        }
        all[j] = v;
    }
    size_t k = na < max ? na : max;
    for (size_t i = 0; i < k; i++) out[i] = all[i];
    return k;
}

int idx_default_active(const struct idx_result *r, size_t n)
{
    for (size_t i = 0; i < n; i++) if (!r[i].fuzzy) return (int)i;
    return -1;
}
