/*
 * kernel/include/tokenizer.h - bounded byte-level BPE tokenizer primitive (issue: tokenizer foundation).
 *
 * What this is: a pure, bounded, allocation-free tokenizer that turns text into token IDs and back.
 * It follows the Qwen3 byte-level BPE shape (vocab + merge table, special tokens BOS/EOS/PAD/UNK) but
 * uses a small SYNTHETIC vocabulary and merge table, sized for testing and for this OS's tight memory
 * limits. Nothing here is the Qwen3-0.6B vocabulary; it is a primitive that proves the shape.
 *
 * What this is NOT: a model, a forward pass, or the real 151k-token Qwen3 tokenizer. The real vocab and
 * merges would not fit in this OS (see docs/kernel/native-inference-prerequisites.md); when they are
 * added they come from a verified model volume, not from host files.
 *
 * Every function is bounded: explicit lengths, no NUL assumptions on input, output buffers sized by the
 * caller, nothing written past them, no dynamic allocation, no libc.
 */
#ifndef AURORA_TOKENIZER_H
#define AURORA_TOKENIZER_H

#include <stdint.h>
#include <stddef.h>

/* ---- limits (all bounded; enlarge only with a memory review) ---- */
#define TOK_MAX_INPUT   256u   /* max input bytes the tokenizer will accept */
#define TOK_MAX_TOKENS  128u   /* max output token IDs */
#define TOK_MAX_PIECE    16u   /* max bytes in a single vocab piece (incl. NUL) */
#define TOK_VOCAB_BYTES 256u   /* one token per byte value */
#define TOK_ID_FIRST     4u    /* first non-special token id */

/* ---- special token ids ---- */
#define TOK_ID_BOS 0u   /* beginning of sequence */
#define TOK_ID_EOS 1u   /* end of sequence */
#define TOK_ID_PAD 2u   /* padding */
#define TOK_ID_UNK 3u   /* unknown / out-of-vocabulary piece */

/* A vocab entry: a piece of text and its id. Pieces are NUL-terminated. */
struct tok_entry {
    const char *piece;
    uint32_t id;
};

/* A BPE merge: adjacent pair (left, right) merges into out. Rank is priority:
 * lower rank is applied first; ties resolve to the leftmost occurrence. */
struct tok_merge {
    uint32_t left;
    uint32_t right;
    uint32_t out;
    uint32_t rank;
};

/* The tokenizer: static tables only, no per-call heap state. */
struct tokenizer {
    const struct tok_entry *vocab;      /* multi-byte piece table (byte tokens are computed) */
    size_t      vocab_size;             /* number of entries in vocab */
    const struct tok_merge *merges;     /* BPE merge table */
    size_t      merge_count;            /* number of entries in merges */
    uint32_t    scratch[TOK_MAX_INPUT]; /* working token-Id sequence (one slot per input byte) */
    size_t      scratch_len;
};

/* Initialise *t with the given tables. Must be called before encode/decode. */
void tokenizer_init(struct tokenizer *t,
                    const struct tok_entry *vocab, size_t vocab_size,
                    const struct tok_merge *merges, size_t merge_count);

/* Encode: read `in_len` bytes from `in` (need not be NUL-terminated, never read
 * past in_len), validate UTF-8, build byte tokens, apply BPE merges, and write
 * token IDs to `out` (capacity `out_cap`). If add_bos, prepend TOK_ID_BOS; if
 * add_eos, append TOK_ID_EOS. Returns the number of IDs written, or a negative
 * error: TOK_E_TOO_LONG, TOK_E_BAD_UTF8, TOK_E_OVERFLOW. Never writes past out. */
int tokenizer_encode(struct tokenizer *t, const char *in, size_t in_len,
                     uint32_t *out, size_t out_cap, int add_bos, int add_eos);

/* Decode: convert `n` token IDs from `ids` back into text in `out` (capacity
 * `out_cap`). PAD is skipped; UNK writes the replacement byte 0xFFFD; a byte
 * token writes that byte; a multi-byte piece writes its text. Returns bytes
 * written (excluding NUL), or a negative error. Never writes past out and never
 * NUL-terminates past out. */
int tokenizer_decode(struct tokenizer *t, const uint32_t *ids, size_t n,
                     char *out, size_t out_cap);

/* Token ID -> piece text: bounded copy of the piece for `id` into `out`
 * (capacity `out_cap`). Returns bytes copied (excluding NUL), or a negative
 * error if `id` is unknown or the buffer is too small. */
int tokenizer_piece(struct tokenizer *t, uint32_t id, char *out, size_t out_cap);

/* Special token id by name ("bos","eos","pad","unk"); TOK_ID_UNK if not found. */
uint32_t tokenizer_special_id(const char *name);

/* ---- error codes (negative) ---- */
enum {
    TOK_E_TOO_LONG = -1,   /* input exceeds TOK_MAX_INPUT or output would exceed out_cap */
    TOK_E_BAD_UTF8 = -2,   /* input is not valid UTF-8 */
    TOK_E_OVERFLOW = -3,   /* internal bounded limit exceeded */
};

#endif /* AURORA_TOKENIZER_H */
