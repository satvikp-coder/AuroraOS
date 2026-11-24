/*
 * kernel/include/index_model.h - the native Index relevance model (issue #8). ASCII port of the specification in
 * aurora-glass/prototype/index-model.js and its unit tests (tests/unit/index.test.js): the browser module is a behavioural reference,
 * this is an independent C implementation. Pure: no allocation, no globals, every buffer bounded, deterministic.
 *
 * Tiers, best first: exact, prefix, word-prefix (every typed word starts some word of the label), substring, keyword, then a conservative
 * typo match used only for APP NAMES. A typo match is a SUGGESTION: it is flagged fuzzy, ranks last and is never the default - Enter does
 * nothing until the person chooses it. Recency only breaks ties inside a tier. Text is printable ASCII only (the input layer's limit);
 * other bytes are dropped by idx_norm. Index results are navigation targets only: matching never executes anything.
 */
#ifndef AURORA_INDEX_MODEL_H
#define AURORA_INDEX_MODEL_H
#include <stddef.h>
#include <stdint.h>

enum { IDX_EXACT = 0, IDX_PREFIX, IDX_WORD_PREFIX, IDX_SUBSTRING, IDX_KEYWORD, IDX_FUZZY, IDX_NONE = -1 };
#define IDX_MAX_QUERY 32          /* characters the search field accepts */
#define IDX_NORM_CAP 64           /* size of a normalised-string buffer */
#define IDX_MIN_FUZZY_LEN 4
#define IDX_MAX_RESULTS 16

/* Lower-case ASCII, collapse runs of whitespace to one space, trim, drop every non-printable or non-ASCII byte. Always NUL-terminates
 * (cap >= 1) and truncates at cap - 1. Returns the length. */
size_t idx_norm(const char *s, char *out, size_t cap);
/* Strongest non-fuzzy tier of the query against label and keywords, or IDX_NONE. An empty query matches everything at IDX_PREFIX. */
int idx_match_tier(const char *query, const char *label, const char *const *keywords, size_t nkw);
/* Optimal-string-alignment distance (insert, delete, substitute, swap of neighbours = 1 each). Returns max + 1 as soon as it must exceed max. */
int idx_edit_distance(const char *a, const char *b, int max);
/* Conservative typo match: query >= 4 chars, no spaces, 1 edit (2 from 8 chars), same first letter, whole words or whole name only, and not
 * already an exact/prefix/substring match. Returns 1 and the distance, or 0. */
int idx_fuzzy(const char *query, const char *name, int *distance);

struct idx_item { const char *key; const char *label; const char *kw[3]; uint8_t is_app; };
struct idx_result { uint16_t item; int8_t tier; uint8_t fuzzy; };

/* Search `items` (n <= 256) for `query`; recent = keys, most recent first. Writes at most `max` results ordered by tier, then recency,
 * then source order; returns the count. Items with is_app also get typo suggestions (flagged fuzzy, last). */
size_t idx_search(const struct idx_item *items, size_t n, const char *query, const char *const *recent, size_t nrecent,
                  struct idx_result *out, size_t max);
/* First non-fuzzy result, or -1 (a typo suggestion is never opened by default). */
int idx_default_active(const struct idx_result *r, size_t n);

#endif
