/*
 * kernel/include/speech_kw.h - Bounded offline keyword command matcher (SPEECH-03).
 *
 * Pure, freestanding, no allocation, no floating point, no libc, no globals.
 * Matches a fixed-point quantized feature vector (energy_q8, zcr_q8, band_ratio_q8, duration_q8)
 * against up to SKW_MAX_TEMPLATES (8) registered keyword templates using L1 distance.
 * Fail-closed: silence, below-threshold confidence, or ambiguous ties return SKW_CMD_NONE.
 * Matching a keyword never executes anything; it returns a typed command ID for Aura/Broker.
 */
#ifndef AURORA_SPEECH_KW_H
#define AURORA_SPEECH_KW_H

#include <stddef.h>
#include <stdint.h>

#define SKW_FEAT_DIM      4u
#define SKW_MAX_TEMPLATES 8u
#define SKW_MAX_DIST      20u   /* max L1 distance over 4 int8 [0..127] dims to accept */
#define SKW_MIN_MARGIN    10u   /* min L1 gap over runner-up command to avoid ambiguity */

enum skw_cmd {
    SKW_CMD_NONE = 0,
    SKW_CMD_HOME = 1,
    SKW_CMD_OPEN_NOTES = 2,
    SKW_CMD_CANCEL_TIMER = 3,
    SKW_CMD_COUNT
};

enum skw_status {
    SKW_OK = 0,
    SKW_E_ARG = -1,
    SKW_E_FULL = -2,
    SKW_E_NO_MATCH = -3,
    SKW_E_AMBIGUOUS = -4,
    SKW_E_SILENCE = -5
};

struct skw_vec {
    int8_t v[SKW_FEAT_DIM];     /* [0..127]: energy_q8, zcr_q8, band_ratio_q8, duration_q8 */
};

struct skw_template {
    uint8_t used;
    uint8_t cmd;                /* enum skw_cmd (1..SKW_CMD_COUNT-1) */
    struct skw_vec vec;
};

struct skw_table {
    struct skw_template t[SKW_MAX_TEMPLATES];
    uint32_t count;
};

struct skw_match {
    uint8_t cmd;                /* SKW_CMD_NONE unless SKW_OK */
    int32_t status;             /* enum skw_status */
    uint32_t best_dist;
    uint32_t second_dist;
};

static inline void skw_table_init(struct skw_table *tbl)
{
    if (!tbl) return;
    tbl->count = 0;
    for (size_t i = 0; i < SKW_MAX_TEMPLATES; i++) {
        tbl->t[i].used = 0;
        tbl->t[i].cmd = SKW_CMD_NONE;
        for (size_t d = 0; d < SKW_FEAT_DIM; d++) tbl->t[i].vec.v[d] = 0;
    }
}

static inline int skw_vec_valid(const struct skw_vec *v)
{
    if (!v) return 0;
    for (size_t d = 0; d < SKW_FEAT_DIM; d++) {
        if (v->v[d] < 0) return 0;
    }
    return 1;
}

static inline int skw_add_template(struct skw_table *tbl, uint8_t cmd, const struct skw_vec *v)
{
    if (!tbl || !skw_vec_valid(v) || cmd == SKW_CMD_NONE || cmd >= SKW_CMD_COUNT) return SKW_E_ARG;
    if (tbl->count >= SKW_MAX_TEMPLATES) return SKW_E_FULL;
    for (size_t i = 0; i < SKW_MAX_TEMPLATES; i++) {
        if (!tbl->t[i].used) {
            tbl->t[i].used = 1;
            tbl->t[i].cmd = cmd;
            tbl->t[i].vec = *v;
            tbl->count++;
            return SKW_OK;
        }
    }
    return SKW_E_FULL;
}

static inline uint32_t skw_l1_dist(const struct skw_vec *a, const struct skw_vec *b)
{
    uint32_t sum = 0;
    for (size_t d = 0; d < SKW_FEAT_DIM; d++) {
        int32_t diff = (int32_t)a->v[d] - (int32_t)b->v[d];
        sum += (uint32_t)(diff < 0 ? -diff : diff);
    }
    return sum;
}

static inline int skw_recognize(const struct skw_table *tbl,
                                const struct skw_vec *in,
                                struct skw_match *out)
{
    if (!out) return SKW_E_ARG;
    out->cmd = SKW_CMD_NONE;
    out->status = SKW_E_ARG;
    out->best_dist = UINT32_MAX;
    out->second_dist = UINT32_MAX;

    if (!tbl || !skw_vec_valid(in)) return SKW_E_ARG;
    if (in->v[0] == 0) {
        out->status = SKW_E_SILENCE;
        return SKW_E_SILENCE;
    }
    if (tbl->count == 0) {
        out->status = SKW_E_NO_MATCH;
        return SKW_E_NO_MATCH;
    }

    uint32_t best_d = UINT32_MAX;
    uint32_t second_d = UINT32_MAX;
    uint8_t best_cmd = SKW_CMD_NONE;

    for (size_t i = 0; i < SKW_MAX_TEMPLATES; i++) {
        if (!tbl->t[i].used) continue;
        uint32_t d = skw_l1_dist(in, &tbl->t[i].vec);
        if (d < best_d) {
            if (best_cmd != SKW_CMD_NONE && best_cmd != tbl->t[i].cmd) {
                second_d = best_d;
            }
            best_d = d;
            best_cmd = tbl->t[i].cmd;
        } else if (tbl->t[i].cmd != best_cmd && d < second_d) {
            second_d = d;
        }
    }

    out->best_dist = best_d;
    out->second_dist = second_d;

    if (best_cmd == SKW_CMD_NONE || best_d > SKW_MAX_DIST) {
        out->status = SKW_E_NO_MATCH;
        return SKW_E_NO_MATCH;
    }
    if (second_d != UINT32_MAX && (second_d - best_d) < SKW_MIN_MARGIN) {
        out->status = SKW_E_AMBIGUOUS;
        return SKW_E_AMBIGUOUS;
    }

    out->cmd = best_cmd;
    out->status = SKW_OK;
    return SKW_OK;
}

static inline const char *skw_cmd_name(uint8_t cmd)
{
    switch (cmd) {
    case SKW_CMD_HOME:         return "home";
    case SKW_CMD_OPEN_NOTES:   return "open_notes";
    case SKW_CMD_CANCEL_TIMER: return "cancel_timer";
    default:                   return "none";
    }
}

#endif /* AURORA_SPEECH_KW_H */
