/*
 * kernel/include/model_trust.h - Model volume trust-root & v2 signature envelope validation.
 *
 * Implements the trust-root separation, downgrade/rollback guard, and fixed-size v2 signature
 * trailer parser specified in docs/kernel/model-auth-ed25519-eval.md.
 *
 * Pure C, freestanding, no allocation, no libc, no globals.
 * Note: Cryptographic Ed25519 verification is supplied via the `mt_verify_fn` callback
 * (e.g. a vetted Monocypher/ref10 verifier); this header does NOT implement Ed25519 math itself
 * and fails closed if `verify_fn` is NULL.
 */
#ifndef AURORA_MODEL_TRUST_H
#define AURORA_MODEL_TRUST_H

#include <stddef.h>
#include <stdint.h>

#if defined(AURORA_MODEL_TRUST_PROD) && defined(AURORA_MODEL_TRUST_TEST)
#error "AURORA_MODEL_TRUST_TEST must never be enabled alongside AURORA_MODEL_TRUST_PROD"
#endif

#define MT_TRAILER_MAGIC0   'A'
#define MT_TRAILER_MAGIC1   'U'
#define MT_TRAILER_MAGIC2   'R'
#define MT_TRAILER_MAGIC3   'S'
#define MT_TRAILER_VERSION  1u
#define MT_TRAILER_BYTES    116u  /* 4 magic + 4 ver + 4 key_id + 4 model_ver + 32 hdr_digest + 64 sig + 4 reserved */
#define MT_MAX_TRUST_KEYS   8u

enum mt_status {
    MT_AUTHENTICATED = 0,
    MT_E_ARG = -1,
    MT_E_TRUNC = -2,
    MT_E_FORMAT = -3,
    MT_E_VERSION = -4,
    MT_E_UNKNOWN_KEY = -5,
    MT_E_REVOKED_KEY = -6,
    MT_E_ROLLBACK = -7,
    MT_E_DIGEST = -8,
    MT_E_BAD_SIG = -9,
    MT_E_NO_VERIFIER = -10
};

struct mt_key_entry {
    uint32_t key_id;
    uint8_t  pubkey[32];
    uint8_t  active;            /* 1 = trusted, 0 = revoked */
};

struct mt_trust_table {
    struct mt_key_entry keys[MT_MAX_TRUST_KEYS];
    uint32_t count;
    uint32_t min_model_version; /* rollback/downgrade floor */
};

struct mt_trailer {
    uint32_t trailer_version;
    uint32_t key_id;
    uint32_t model_version;
    uint8_t  hdr_digest[32];
    uint8_t  sig[64];
};

/* Callback returning 0 iff `sig[64]` is a valid signature over `msg[msg_len]` under `pubkey[32]`. */
typedef int (*mt_verify_fn)(const uint8_t sig[64], const uint8_t *msg, size_t msg_len, const uint8_t pubkey[32]);

static inline uint32_t mt_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int mt_sig_noncanonical_zero(const uint8_t sig[64])
{
    uint8_t or_r = 0, or_s = 0;
    for (size_t i = 0; i < 32; i++) {
        or_r |= sig[i];
        or_s |= sig[32 + i];
    }
    if (or_r == 0 || or_s == 0) return 1;
    /* In Ed25519, S < L < 2^253, so the top 3 bits of sig[63] must be 0. */
    if ((sig[63] & 0xE0u) != 0) return 1;
    return 0;
}

static inline int mt_parse_trailer(const uint8_t *buf, size_t len, struct mt_trailer *out)
{
    if (!buf || !out) return MT_E_ARG;
    if (len < MT_TRAILER_BYTES) return MT_E_TRUNC;
    if (len != MT_TRAILER_BYTES) return MT_E_FORMAT;
    if (buf[0] != MT_TRAILER_MAGIC0 || buf[1] != MT_TRAILER_MAGIC1 ||
        buf[2] != MT_TRAILER_MAGIC2 || buf[3] != MT_TRAILER_MAGIC3) return MT_E_FORMAT;

    uint32_t tver = mt_get_le32(buf + 4);
    if (tver != MT_TRAILER_VERSION) return MT_E_VERSION;

    uint32_t reserved = mt_get_le32(buf + 112);
    if (reserved != 0) return MT_E_FORMAT;

    out->trailer_version = tver;
    out->key_id = mt_get_le32(buf + 8);
    out->model_version = mt_get_le32(buf + 12);
    for (size_t i = 0; i < 32; i++) out->hdr_digest[i] = buf[16 + i];
    for (size_t i = 0; i < 64; i++) out->sig[i] = buf[48 + i];
    return MT_AUTHENTICATED;
}

static inline int mt_verify_envelope(const struct mt_trust_table *trust,
                                     const uint8_t expected_hdr_digest[32],
                                     const uint8_t *trailer_buf,
                                     size_t trailer_len,
                                     mt_verify_fn verify_fn,
                                     struct mt_trailer *out_parsed)
{
    if (!trust || !expected_hdr_digest || !trailer_buf) return MT_E_ARG;
    if (trust->count > MT_MAX_TRUST_KEYS) return MT_E_ARG;

    struct mt_trailer tr;
    int rc = mt_parse_trailer(trailer_buf, trailer_len, &tr);
    if (rc != MT_AUTHENTICATED) return rc;
    if (out_parsed) *out_parsed = tr;

    if (tr.model_version < trust->min_model_version) return MT_E_ROLLBACK;

    uint8_t diff = 0;
    for (size_t i = 0; i < 32; i++) diff |= (uint8_t)(tr.hdr_digest[i] ^ expected_hdr_digest[i]);
    if (diff != 0) return MT_E_DIGEST;

    if (mt_sig_noncanonical_zero(tr.sig)) return MT_E_BAD_SIG;

    const struct mt_key_entry *found = 0;
    for (uint32_t i = 0; i < trust->count; i++) {
        if (trust->keys[i].key_id == tr.key_id) {
            found = &trust->keys[i];
            break;
        }
    }
    if (!found) return MT_E_UNKNOWN_KEY;
    if (!found->active) return MT_E_REVOKED_KEY;

    if (!verify_fn) return MT_E_NO_VERIFIER;
    if (verify_fn(tr.sig, tr.hdr_digest, 32, found->pubkey) != 0) return MT_E_BAD_SIG;

    return MT_AUTHENTICATED;
}

#endif /* AURORA_MODEL_TRUST_H */
