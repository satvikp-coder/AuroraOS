/*
 * kernel/include/pin_lock.h - Native PIN & session lock state machine (Issue #11).
 *
 * Pure C, freestanding, no allocation, no libc, no globals.
 * Uses salted SHA-256 verification and constant-time digest comparison (sha256_equal).
 * Enforces PIN length [4..8] ASCII digits, exponential lockout backoff after failed
 * attempts (up to PIN_MAX_ATTEMPTS = 5), monotonic clock backwards guard, and fail-closed
 * locked state until explicitly unlocked.
 */
#ifndef AURORA_PIN_LOCK_H
#define AURORA_PIN_LOCK_H

#include <stddef.h>
#include <stdint.h>
#include <sha256.h>

#define PIN_MIN_LEN       4u
#define PIN_MAX_LEN       8u
#define PIN_SALT_LEN      16u
#define PIN_MAX_ATTEMPTS  5u
#define PIN_BASE_LOCK_MS  1000u
#define PIN_MAX_LOCK_MS   60000u
#define PIN_KDF_ITERATIONS 10000u
#define PIN_REC_BYTES     96u

enum pin_state {
    PIN_STATE_UNSET = 0,
    PIN_STATE_LOCKED,
    PIN_STATE_UNLOCKED,
    PIN_STATE_LOCKOUT
};

enum pin_err {
    PIN_OK = 0,
    PIN_E_ARG = -1,
    PIN_E_FORMAT = -2,
    PIN_E_STATE = -3,
    PIN_E_BAD_PIN = -4,
    PIN_E_LOCKOUT = -5
};

struct pin_lock {
    uint8_t  state;
    uint8_t  salt[PIN_SALT_LEN];
    uint8_t  digest[32];
    uint32_t failed_attempts;
    uint64_t lockout_until_ms;
    uint64_t last_now_ms;
};

static inline void pin_init(struct pin_lock *pl)
{
    if (!pl) return;
    uint8_t *p = (uint8_t *)pl;
    for (size_t i = 0; i < sizeof(*pl); ++i) p[i] = 0;
    pl->state = PIN_STATE_UNSET;
}

static inline int pin_valid_digits(const char *digits, size_t len)
{
    if (!digits || len < PIN_MIN_LEN || len > PIN_MAX_LEN) return 0;
    for (size_t i = 0; i < len; ++i) {
        if (digits[i] < '0' || digits[i] > '9') return 0;
    }
    return 1;
}

static inline void pin_hash(const uint8_t salt[PIN_SALT_LEN],
                            const char *digits,
                            size_t len,
                            uint8_t out[32])
{
    static const uint8_t domain[13] = { 'A','U','R','O','R','A','-','P','I','N','-','V','1' };
    uint8_t kdf_salt[sizeof(domain) + PIN_SALT_LEN];
    for (size_t i = 0; i < sizeof(domain); ++i) kdf_salt[i] = domain[i];
    for (size_t i = 0; i < PIN_SALT_LEN; ++i) kdf_salt[sizeof(domain) + i] = salt[i];
    pbkdf2_hmac_sha256(digits, len, kdf_salt, sizeof(kdf_salt), PIN_KDF_ITERATIONS, out);
    for (size_t i = 0; i < sizeof(kdf_salt); ++i) kdf_salt[i] = 0;
}

/* Configure initial PIN and enter LOCKED state (fail-closed). */
static inline int pin_set(struct pin_lock *pl,
                          const uint8_t salt[PIN_SALT_LEN],
                          const char *digits,
                          size_t len)
{
    if (!pl || !salt) return PIN_E_ARG;
    if (pl->state != PIN_STATE_UNSET) return PIN_E_STATE;
    if (!pin_valid_digits(digits, len)) return PIN_E_FORMAT;

    for (size_t i = 0; i < PIN_SALT_LEN; ++i) pl->salt[i] = salt[i];
    pin_hash(pl->salt, digits, len, pl->digest);
    pl->failed_attempts = 0;
    pl->lockout_until_ms = 0;
    pl->last_now_ms = 0;
    pl->state = PIN_STATE_LOCKED;
    return PIN_OK;
}

static inline void pin_tick(struct pin_lock *pl, uint64_t now_ms)
{
    if (!pl) return;
    if (now_ms < pl->last_now_ms) return; /* monotonic clock backwards guard */
    pl->last_now_ms = now_ms;
    if (pl->state == PIN_STATE_LOCKOUT && now_ms >= pl->lockout_until_ms) {
        pl->state = PIN_STATE_LOCKED;
    }
}

static inline int pin_lock_now(struct pin_lock *pl)
{
    if (!pl) return PIN_E_ARG;
    if (pl->state == PIN_STATE_UNSET) return PIN_E_STATE;
    if (pl->state == PIN_STATE_UNLOCKED) pl->state = PIN_STATE_LOCKED;
    return PIN_OK;
}

static inline int pin_unlock(struct pin_lock *pl,
                             uint64_t now_ms,
                             const char *digits,
                             size_t len)
{
    if (!pl || !digits) return PIN_E_ARG;
    if (pl->state == PIN_STATE_UNSET) return PIN_E_STATE;
    pin_tick(pl, now_ms);
    if (pl->state == PIN_STATE_LOCKOUT) return PIN_E_LOCKOUT;
    if (!pin_valid_digits(digits, len)) return PIN_E_FORMAT;

    uint8_t cand[32];
    pin_hash(pl->salt, digits, len, cand);
    int match = sha256_equal(cand, pl->digest);
    for (size_t i = 0; i < sizeof(cand); ++i) cand[i] = 0;

    if (match) {
        pl->failed_attempts = 0;
        pl->lockout_until_ms = 0;
        pl->state = PIN_STATE_UNLOCKED;
        return PIN_OK;
    }

    pl->failed_attempts++;
    if (pl->failed_attempts >= PIN_MAX_ATTEMPTS) {
        uint32_t over = pl->failed_attempts - PIN_MAX_ATTEMPTS;
        uint64_t dur = PIN_BASE_LOCK_MS;
        for (uint32_t i = 0; i < over && i < 6u; ++i) dur *= 2u;
        if (dur > PIN_MAX_LOCK_MS) dur = PIN_MAX_LOCK_MS;
        uint64_t base = now_ms > pl->last_now_ms ? now_ms : pl->last_now_ms;
        pl->lockout_until_ms = base + dur;
        pl->state = PIN_STATE_LOCKOUT;
        return PIN_E_LOCKOUT;
    }
    return PIN_E_BAD_PIN;
}

static inline int pin_change(struct pin_lock *pl,
                             const char *old_digits,
                             size_t old_len,
                             const uint8_t new_salt[PIN_SALT_LEN],
                             const char *new_digits,
                             size_t new_len)
{
    if (!pl || !old_digits || !new_salt || !new_digits) return PIN_E_ARG;
    if (pl->state != PIN_STATE_UNLOCKED) return PIN_E_STATE;
    if (!pin_valid_digits(old_digits, old_len) || !pin_valid_digits(new_digits, new_len))
        return PIN_E_FORMAT;

    uint8_t cand[32];
    pin_hash(pl->salt, old_digits, old_len, cand);
    int match = sha256_equal(cand, pl->digest);
    for (size_t i = 0; i < sizeof(cand); ++i) cand[i] = 0;
    if (!match) return PIN_E_BAD_PIN;

    for (size_t i = 0; i < PIN_SALT_LEN; ++i) pl->salt[i] = new_salt[i];
    pin_hash(pl->salt, new_digits, new_len, pl->digest);
    pl->failed_attempts = 0;
    return PIN_OK;
}

static inline void pin_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t pin_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Serialize enrolled PIN state + failed attempt counter with SHA-256 integrity trailer. */
static inline int pin_export(const struct pin_lock *pl, uint8_t out[PIN_REC_BYTES])
{
    if (!pl || !out) return PIN_E_ARG;
    if (pl->state == PIN_STATE_UNSET) return PIN_E_STATE;

    out[0] = 'A'; out[1] = 'P'; out[2] = 'I'; out[3] = 'N';
    pin_put_le32(out + 4, 1u);
    pin_put_le32(out + 8, PIN_KDF_ITERATIONS);
    pin_put_le32(out + 12, pl->failed_attempts);
    for (size_t i = 0; i < PIN_SALT_LEN; ++i) out[16 + i] = pl->salt[i];
    for (size_t i = 0; i < 32; ++i) out[32 + i] = pl->digest[i];

    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, out, 64);
    sha256_final(&s, out + 64);
    return PIN_OK;
}

/* Restore persisted PIN record; always fails closed into PIN_STATE_LOCKED on valid data,
 * and rejects any malformed/corrupted record without unlocking. */
static inline int pin_import(struct pin_lock *pl, const uint8_t rec[PIN_REC_BYTES], uint64_t now_ms)
{
    if (!pl || !rec) return PIN_E_ARG;
    if (rec[0] != 'A' || rec[1] != 'P' || rec[2] != 'I' || rec[3] != 'N') return PIN_E_FORMAT;
    if (pin_get_le32(rec + 4) != 1u || pin_get_le32(rec + 8) != PIN_KDF_ITERATIONS) return PIN_E_FORMAT;

    uint8_t want[32];
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, rec, 64);
    sha256_final(&s, want);
    int ok = sha256_equal(want, rec + 64);
    for (size_t i = 0; i < sizeof(want); ++i) want[i] = 0;
    if (!ok) return PIN_E_FORMAT;

    for (size_t i = 0; i < PIN_SALT_LEN; ++i) pl->salt[i] = rec[16 + i];
    for (size_t i = 0; i < 32; ++i) pl->digest[i] = rec[32 + i];
    pl->failed_attempts = pin_get_le32(rec + 12);
    pl->last_now_ms = now_ms;
    if (pl->failed_attempts >= PIN_MAX_ATTEMPTS) {
        pl->lockout_until_ms = now_ms + PIN_BASE_LOCK_MS;
        pl->state = PIN_STATE_LOCKOUT;
    } else {
        pl->lockout_until_ms = 0;
        pl->state = PIN_STATE_LOCKED;
    }
    return PIN_OK;
}

#endif /* AURORA_PIN_LOCK_H */
