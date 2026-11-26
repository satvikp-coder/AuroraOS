/*
 * kernel/proc/tokenizer.c - bounded byte-level BPE tokenizer primitive. See kernel/include/tokenizer.h.
 * Pure C, no libc, no dynamic allocation, every buffer bounded by the caller.
 */
#include <tokenizer.h>

/* ---- tiny helpers (no libc) ---- */
static int toklen(const char *s)
{
    if (!s) return -1;
    for (int n = 0; n < (int)TOK_MAX_PIECE; n++) if (!s[n]) return n;
    return -1;
}
static int teq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* ---- UTF-8 validation ---- */
/* Returns the number of bytes consumed for one code point starting at p[0], or 0 if invalid.
 * Rejects overlong encodings (RFC 3629) and surrogate/above-U+10FFFF ranges.
 * Never reads past end. */
static int utf8_len(const uint8_t *p, size_t avail)
{
    if (avail == 0) return 0;
    uint8_t c = p[0];
    if (c < 0x80) return 1;                      /* ASCII */
    if ((c & 0xE0) == 0xC0) {                    /* 2 bytes: U+0080..U+07FF */
        if (avail < 2) return 0;
        if ((p[1] & 0xC0) != 0x80) return 0;
        if (c < 0xC2) return 0;                  /* 0xC0/0xC1 are overlong prefixes */
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {                    /* 3 bytes: U+0800..U+FFFF */
        if (avail < 3) return 0;
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && p[1] < 0xA0) return 0;  /* overlong: U+0000..U+07FF in 3 bytes */
        if (c == 0xED && p[1] >= 0xA0) return 0; /* surrogate halves U+D800..U+DFFF */
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {                    /* 4 bytes: U+10000..U+10FFFF */
        if (avail < 4) return 0;
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0 && p[1] < 0x90) return 0;  /* overlong: U+0000..U+FFFF in 4 bytes */
        if (c > 0xF4) return 0;                  /* 0xF5-0xF7: above U+10FFFF */
        if (c == 0xF4 && p[1] >= 0x90) return 0; /* above U+10FFFF */
        return 4;
    }
    return 0;                                    /* invalid lead or continuation byte alone */
}

void tokenizer_init(struct tokenizer *t,
                    const struct tok_entry *vocab, size_t vocab_size,
                    const struct tok_merge *merges, size_t merge_count)
{
    if (!t) return;
    t->vocab = vocab;
    t->vocab_size = vocab_size;
    t->merges = merges;
    t->merge_count = merge_count;
    t->scratch_len = 0;
    t->valid = 0;
    if (vocab_size > TOK_MAX_VOCAB || merge_count > TOK_MAX_MERGES ||
        (vocab_size && !vocab) || (merge_count && !merges)) return;
    for (size_t i = 0; i < vocab_size; i++) {
        if (vocab[i].id < TOK_ID_FIRST || vocab[i].id >= TOK_BYTE_ID_BASE || toklen(vocab[i].piece) <= 0) return;
        for (size_t j = 0; j < i; j++) if (vocab[j].id == vocab[i].id) return;
    }
    t->valid = 1;
}

/* Find the piece id for a byte token (0..255).
 * If the vocab has a single-byte entry for `b` use its id; otherwise compute
 * a byte-token id in [TOK_BYTE_ID_BASE, TOK_BYTE_ID_BASE + TOK_VOCAB_BYTES) so every byte round-trips. */
static uint32_t byte_id(const struct tokenizer *t, uint8_t b)
{
    for (size_t i = 0; i < t->vocab_size; i++) {
        const char *p = t->vocab[i].piece;
        if (p && p[0] && p[0] == (char)b && p[1] == 0) return t->vocab[i].id;
    }
    return TOK_BYTE_ID_BASE + b;
}

/* Given two adjacent token ids l, r, find the merge output, or TOK_ID_UNK if none.
 * The merge with the lowest rank wins; ties resolve to the leftmost occurrence. */
static uint32_t merge_find(const struct tokenizer *t, uint32_t l, uint32_t r, uint32_t *best_rank)
{
    uint32_t out = TOK_ID_UNK;
    uint32_t best = UINT32_MAX;
    for (size_t i = 0; i < t->merge_count; i++) {
        if (t->merges[i].left == l && t->merges[i].right == r) {
            if (t->merges[i].rank < best) {
                best = t->merges[i].rank;
                out = t->merges[i].out;
            }
        }
    }
    if (best_rank) *best_rank = best;
    return out;
}

int tokenizer_encode(struct tokenizer *t, const char *in, size_t in_len,
                     uint32_t *out, size_t out_cap, int add_bos, int add_eos)
{
    if (!t || !t->valid || !in || !out) return TOK_E_OVERFLOW;
    if (in_len > TOK_MAX_INPUT) return TOK_E_TOO_LONG;

    /* Validate UTF-8 over the whole input. */
    size_t pos = 0;
    while (pos < in_len) {
        int n = utf8_len((const uint8_t *)(in + pos), in_len - pos);
        if (n == 0) return TOK_E_BAD_UTF8;
        pos += (size_t)n;
    }

    /* Build initial byte tokens into scratch. */
    t->scratch_len = 0;
    for (size_t i = 0; i < in_len; i++) {
        uint32_t id = byte_id(t, (uint8_t)in[i]);
        if (t->scratch_len >= TOK_MAX_INPUT) return TOK_E_TOO_LONG;
        t->scratch[t->scratch_len++] = id;
    }

    /* BPE: repeatedly apply the lowest-rank merge that exists anywhere in the current sequence. */
    for (;;) {
        uint32_t best_rank = UINT32_MAX;
        size_t best_i = (size_t)-1;
        uint32_t best_out = TOK_ID_UNK;
        for (size_t i = 0; i + 1 < t->scratch_len; i++) {
            uint32_t r = 0;
            uint32_t o = merge_find(t, t->scratch[i], t->scratch[i + 1], &r);
            if (o != TOK_ID_UNK && r < best_rank) {
                best_rank = r;
                best_i = i;
                best_out = o;
            }
        }
        if (best_i == (size_t)-1) break;           /* no more merges */
        t->scratch[best_i] = best_out;
        for (size_t i = best_i + 1; i + 1 < t->scratch_len; i++) { t->scratch[i] = t->scratch[i + 1]; }
        t->scratch_len--;
    }

    /* Write output tokens, with optional BOS/EOS. */
    size_t need = t->scratch_len;
    if (add_bos) need++;
    if (add_eos) need++;
    if (need > TOK_MAX_TOKENS || need > out_cap) return TOK_E_TOO_LONG;

    size_t w = 0;
    if (add_bos) out[w++] = TOK_ID_BOS;
    for (size_t i = 0; i < t->scratch_len; i++) { out[w++] = t->scratch[i]; }
    if (add_eos) out[w++] = TOK_ID_EOS;
    return (int)w;
}

int tokenizer_decode(struct tokenizer *t, const uint32_t *ids, size_t n,
                     char *out, size_t out_cap)
{
    if (!t || !t->valid || !ids || !out) return TOK_E_OVERFLOW;
    if (n > TOK_MAX_TOKENS) return TOK_E_TOO_LONG;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t id = ids[i];
        if (id == TOK_ID_PAD) continue;
        if (id == TOK_ID_UNK) {
            if (w + 3 > out_cap) return TOK_E_TOO_LONG;
            out[w++] = (char)0xEF; out[w++] = (char)0xBF; out[w++] = (char)0xBD; /* U+FFFD */
            continue;
        }
        if (id == TOK_ID_BOS || id == TOK_ID_EOS) continue; /* special: nothing written */
        /* Multi-byte piece lookup. */
        const char *piece = 0;
        for (size_t k = 0; k < t->vocab_size; k++) {
            if (t->vocab[k].id == id) { piece = t->vocab[k].piece; break; }
        }
        if (piece) {
            int l = toklen(piece);
            if (w + (size_t)l > out_cap) return TOK_E_TOO_LONG;
            for (int j = 0; j < l; j++) { out[w++] = piece[j]; }
        } else {
            /* Only the reserved fallback byte range is valid here. */
            if (id < TOK_BYTE_ID_BASE || id >= TOK_BYTE_ID_BASE + TOK_VOCAB_BYTES) return TOK_E_UNKNOWN;
            if (w + 1 > out_cap) return TOK_E_TOO_LONG;
            out[w++] = (char)(id - TOK_BYTE_ID_BASE);
        }
    }
    return (int)w;
}

int tokenizer_piece(struct tokenizer *t, uint32_t id, char *out, size_t out_cap)
{
    if (!t || !t->valid || !out) return TOK_E_OVERFLOW;
    if (id >= TOK_BYTE_ID_BASE && id < TOK_BYTE_ID_BASE + TOK_VOCAB_BYTES) {
        if (out_cap < 2) return TOK_E_TOO_LONG;
        out[0] = (char)(id - TOK_BYTE_ID_BASE);
        out[1] = 0;
        return 1;
    }
    if (id == TOK_ID_BOS) { const char *s = "<bos>"; int l = toklen(s);
        if ((size_t)l + 1 > out_cap) return TOK_E_TOO_LONG;
        for (int i = 0; i < l; i++) { out[i] = s[i]; } out[l] = 0; return l; }
    if (id == TOK_ID_EOS) { const char *s = "<eos>"; int l = toklen(s);
        if ((size_t)l + 1 > out_cap) return TOK_E_TOO_LONG;
        for (int i = 0; i < l; i++) { out[i] = s[i]; } out[l] = 0; return l; }
    if (id == TOK_ID_PAD) { const char *s = "<pad>"; int l = toklen(s);
        if ((size_t)l + 1 > out_cap) return TOK_E_TOO_LONG;
        for (int i = 0; i < l; i++) { out[i] = s[i]; } out[l] = 0; return l; }
    if (id == TOK_ID_UNK) { const char *s = "<|unk|>"; int l = toklen(s);
        if ((size_t)l + 1 > out_cap) return TOK_E_TOO_LONG;
        for (int i = 0; i < l; i++) { out[i] = s[i]; } out[l] = 0; return l; }
    for (size_t k = 0; k < t->vocab_size; k++) {
        if (t->vocab[k].id == id) {
            const char *s = t->vocab[k].piece;
            int l = toklen(s);
            if ((size_t)l + 1 > out_cap) return TOK_E_TOO_LONG;
            for (int i = 0; i < l; i++) { out[i] = s[i]; } out[l] = 0; return l;
        }
    }
    return TOK_E_TOO_LONG; /* unknown id */
}

uint32_t tokenizer_special_id(const char *name)
{
    if (!name) return TOK_ID_UNK;
    if (teq(name, "bos")) return TOK_ID_BOS;
    if (teq(name, "eos")) return TOK_ID_EOS;
    if (teq(name, "pad")) return TOK_ID_PAD;
    if (teq(name, "unk")) return TOK_ID_UNK;
    return TOK_ID_UNK;
}
