/* tests/kernel-host/tokenizer_test.c - host tests for kernel/proc/tokenizer.c:
 * bounded byte-level BPE tokenizer. Tests encode/decode round trips, UTF-8
 * validation (incl. overlong rejection), BPE merge ordering, buffer bounds,
 * and special token handling. Standalone: not part of the kernel build yet
 * (no consumer). Compile with: cc tokenizer_test.c tokenizer.c -o tokenizer_test */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tokenizer.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

/* ---- a tiny synthetic vocab + merge table for testing ---- */
/* Byte tokens: id 4 = 'a', 5 = 'b', 6 = 'c', 7 = ' ' */
static const struct tok_entry TEST_VOCAB[] = {
    { "a", 4u }, { "b", 5u }, { "c", 6u }, { " ", 7u },
    { "ab", 8u }, { "bc", 9u }, { "abc", 10u },
};
#define TEST_VOCAB_SIZE (sizeof TEST_VOCAB / sizeof TEST_VOCAB[0])

/* Merges: 'a'+'b'->'ab'(rank 1), 'b'+'c'->'bc'(rank 2), 'ab'+'c'->'abc'(rank 3) */
static const struct tok_merge TEST_MERGES[] = {
    { 4u, 5u, 8u, 1u },   /* a + b -> ab, rank 1 (applied first) */
    { 5u, 6u, 9u, 2u },   /* b + c -> bc, rank 2 */
    { 8u, 6u, 10u, 3u },  /* ab + c -> abc, rank 3 */
};
#define TEST_MERGE_COUNT (sizeof TEST_MERGES / sizeof TEST_MERGES[0])

static struct tokenizer TOK;

static void setup(void)
{
    tokenizer_init(&TOK, TEST_VOCAB, TEST_VOCAB_SIZE, TEST_MERGES, TEST_MERGE_COUNT);
}

static int encode_all(const char *in, size_t len, uint32_t *out, size_t cap, int bos, int eos)
{
    return tokenizer_encode(&TOK, in, len, out, cap, bos, eos);
}

int main(void)
{
    uint32_t out[TOK_MAX_TOKENS];
    char dec[256];
    int rc;

    setup();

    /* ---- encode/decode round trip: ASCII text ---- */
    rc = encode_all("abc", 3, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("encode abc returns 1 token (abc merge)", rc == 1 && out[0] == 10u);
    rc = tokenizer_decode(&TOK, out, (size_t)rc, dec, sizeof dec);
    CHECK("decode abc round trip", rc == 3 && memcmp(dec, "abc", 3) == 0);

    /* ---- BPE merge ordering: lowest rank first ----
     * "abc": possible merges are a+b->ab (rank1) then ab+c->abc (rank3).
     * b+c->bc (rank2) is NOT applicable after a+b merges because b is consumed. */
    rc = encode_all("abc", 3, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("abc uses rank-1 merge (ab) then rank-3 (abc)", rc == 1 && out[0] == 10u);

    /* "bc": b+c->bc (rank 2) */
    rc = encode_all("bc", 2, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("bc uses b+c->bc merge", rc == 1 && out[0] == 9u);

    /* "ab": a+b->ab (rank 1) */
    rc = encode_all("ab", 2, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("ab uses a+b->ab merge", rc == 1 && out[0] == 8u);

    /* "a b c": spaces prevent merges across words */
    rc = encode_all("a b c", 5, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("a b c yields 5 byte tokens", rc == 5 &&
          out[0] == 4u && out[1] == 7u && out[2] == 5u && out[3] == 7u && out[4] == 6u);

    /* ---- BOS/EOS ---- */
    rc = encode_all("ab", 2, out, TOK_MAX_TOKENS, 1, 1);
    CHECK("ab with BOS/EOS returns 3 tokens", rc == 3 &&
          out[0] == TOK_ID_BOS && out[1] == 8u && out[2] == TOK_ID_EOS);

    /* ---- decode: special tokens ---- */
    uint32_t specials[] = { TOK_ID_BOS, TOK_ID_EOS, TOK_ID_PAD, TOK_ID_UNK };
    rc = tokenizer_decode(&TOK, specials, 4, dec, sizeof dec);
    CHECK("decode specials: PAD skipped, UNK -> U+FFFD", rc == 3 &&
          (unsigned char)dec[0] == 0xEF && (unsigned char)dec[1] == 0xBF && (unsigned char)dec[2] == 0xBD);

    /* ---- tokenizer_piece ---- */
    char piece[16];
    rc = tokenizer_piece(&TOK, 8u, piece, sizeof piece);
    CHECK("piece(8) == 'ab'", rc == 2 && memcmp(piece, "ab", 2) == 0);
    rc = tokenizer_piece(&TOK, TOK_ID_BOS, piece, sizeof piece);
    CHECK("piece(BOS) == '<bos>'", rc == 5 && memcmp(piece, "<bos>", 5) == 0);
    rc = tokenizer_piece(&TOK, 999u, piece, sizeof piece);
    CHECK("piece(unknown id) returns error", rc < 0);

    /* ---- tokenizer_special_id ---- */
    CHECK("special_id bos", tokenizer_special_id("bos") == TOK_ID_BOS);
    CHECK("special_id eos", tokenizer_special_id("eos") == TOK_ID_EOS);
    CHECK("special_id pad", tokenizer_special_id("pad") == TOK_ID_PAD);
    CHECK("special_id unk", tokenizer_special_id("unk") == TOK_ID_UNK);
    CHECK("special_id unknown", tokenizer_special_id("none") == TOK_ID_UNK);
    CHECK("special_id NULL", tokenizer_special_id(NULL) == TOK_ID_UNK);

    /* ---- UTF-8 validation: overlong encodings rejected ---- */
    /* 2-byte overlong: U+0041 ('A') as 0xC1 0x81 (should be just 0x41) */
    rc = encode_all("\xC1\x81", 2, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("reject 2-byte overlong U+0041", rc == TOK_E_BAD_UTF8);

    /* 3-byte overlong: U+0041 as 0xE0 0x82 0x81 */
    rc = encode_all("\xE0\x82\x81", 3, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("reject 3-byte overlong U+0041", rc == TOK_E_BAD_UTF8);

    /* 4-byte overlong: U+0041 as 0xF0 0x80 0x82 0x81 */
    rc = encode_all("\xF0\x80\x82\x81", 4, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("reject 4-byte overlong U+0041", rc == TOK_E_BAD_UTF8);

    /* Surrogate: U+D800 as 0xED 0xA0 0x80 */
    rc = encode_all("\xED\xA0\x80", 3, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("reject surrogate U+D800", rc == TOK_E_BAD_UTF8);

    /* Above U+10FFFF: 0xF4 0x90 0x80 0x80 */
    rc = encode_all("\xF4\x90\x80\x80", 4, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("reject above U+10FFFF", rc == TOK_E_BAD_UTF8);

    /* Valid multi-byte UTF-8: U+4E2D (CJK) as 0xE4 0xB8 0xAD */
    rc = encode_all("\xE4\xB8\xAD", 3, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("accept valid 3-byte UTF-8", (rc != TOK_E_BAD_UTF8) && (rc > 0));

    /* ---- buffer bounds: input too long ---- */
    char long_in[300];
    memset(long_in, 'a', sizeof long_in);
    rc = encode_all(long_in, sizeof long_in, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("reject input > TOK_MAX_INPUT", rc == TOK_E_TOO_LONG);

    /* ---- buffer bounds: output too small ---- */
    rc = encode_all("abc", 3, out, 0, 0, 0);
    CHECK("reject output cap too small", rc == TOK_E_TOO_LONG);

    /* ---- null arguments ---- */
    rc = tokenizer_encode(NULL, "a", 1, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("encode NULL tokenizer returns error", rc < 0);
    rc = tokenizer_encode(&TOK, NULL, 1, out, TOK_MAX_TOKENS, 0, 0);
    CHECK("encode NULL input returns error", rc < 0);
    rc = tokenizer_encode(&TOK, "a", 1, NULL, TOK_MAX_TOKENS, 0, 0);
    CHECK("encode NULL output returns error", rc < 0);
    rc = tokenizer_decode(NULL, out, 1, dec, sizeof dec);
    CHECK("decode NULL tokenizer returns error", rc < 0);

    printf("tokenizer_test: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
