/* tests/kernel-host/pin_lock_test.c - host tests for kernel/include/pin_lock.h */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pin_lock.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

int main(void)
{
    struct pin_lock pl;
    uint8_t salt1[PIN_SALT_LEN], salt2[PIN_SALT_LEN];
    for (size_t i = 0; i < PIN_SALT_LEN; ++i) {
        salt1[i] = (uint8_t)(0x11u + i);
        salt2[i] = (uint8_t)(0x55u + i);
    }

    pin_init(&pl);
    CHECK("init: UNSET", pl.state == PIN_STATE_UNSET);
    CHECK("unlock while UNSET refused", pin_unlock(&pl, 100, "1234", 4) == PIN_E_STATE);
    CHECK("lock while UNSET refused", pin_lock_now(&pl) == PIN_E_STATE);

    /* Format validation */
    CHECK("null salt refused", pin_set(&pl, 0, "1234", 4) == PIN_E_ARG);
    CHECK("short PIN refused", pin_set(&pl, salt1, "123", 3) == PIN_E_FORMAT);
    CHECK("long PIN refused", pin_set(&pl, salt1, "123456789", 9) == PIN_E_FORMAT);
    CHECK("non-digit PIN refused", pin_set(&pl, salt1, "12a4", 4) == PIN_E_FORMAT);

    /* Set valid PIN -> starts LOCKED */
    CHECK("set valid 6-digit PIN -> LOCKED", pin_set(&pl, salt1, "482910", 6) == PIN_OK && pl.state == PIN_STATE_LOCKED);
    CHECK("double pin_set refused", pin_set(&pl, salt1, "1234", 4) == PIN_E_STATE);

    /* Wrong PINs up to lockout */
    for (uint32_t i = 0; i < PIN_MAX_ATTEMPTS - 1; ++i) {
        CHECK("wrong PIN returns BAD_PIN", pin_unlock(&pl, 1000 + i * 10, "000000", 6) == PIN_E_BAD_PIN);
        CHECK("still LOCKED before threshold", pl.state == PIN_STATE_LOCKED);
    }
    CHECK("5th wrong PIN triggers LOCKOUT", pin_unlock(&pl, 2000, "000000", 6) == PIN_E_LOCKOUT && pl.state == PIN_STATE_LOCKOUT);
    CHECK("even right PIN refused during LOCKOUT", pin_unlock(&pl, 2500, "482910", 6) == PIN_E_LOCKOUT);

    /* Monotonic clock backwards must not clear lockout */
    pin_tick(&pl, 500);
    CHECK("clock backwards keeps LOCKOUT", pl.state == PIN_STATE_LOCKOUT);

    /* Expiry unlocks lockout -> LOCKED, then right PIN unlocks */
    pin_tick(&pl, 3000);
    CHECK("lockout expires to LOCKED", pl.state == PIN_STATE_LOCKED);
    CHECK("correct PIN unlocks", pin_unlock(&pl, 3000, "482910", 6) == PIN_OK && pl.state == PIN_STATE_UNLOCKED && pl.failed_attempts == 0);

    /* Change PIN requires UNLOCKED and matching old PIN */
    CHECK("change PIN with wrong old PIN fails", pin_change(&pl, "999999", 6, salt2, "7777", 4) == PIN_E_BAD_PIN);
    CHECK("change PIN with bad new format fails", pin_change(&pl, "482910", 6, salt2, "12", 2) == PIN_E_FORMAT);
    CHECK("change PIN succeeds", pin_change(&pl, "482910", 6, salt2, "7777", 4) == PIN_OK);

    /* Lock now and verify new PIN works and old PIN fails */
    CHECK("lock_now locks session", pin_lock_now(&pl) == PIN_OK && pl.state == PIN_STATE_LOCKED);
    CHECK("change PIN while LOCKED refused", pin_change(&pl, "7777", 4, salt1, "1234", 4) == PIN_E_STATE);
    CHECK("old PIN rejected after change", pin_unlock(&pl, 4000, "482910", 6) == PIN_E_BAD_PIN);
    CHECK("new PIN unlocks", pin_unlock(&pl, 4100, "7777", 4) == PIN_OK && pl.state == PIN_STATE_UNLOCKED);

    /* PBKDF2-HMAC-SHA256 RFC 6070-equivalent test vectors (iterations = 1 and 2) */
    uint8_t kdf[32];
    static const uint8_t exp_c1[32] = {
        0x12,0x0f,0xb6,0xcf,0xfc,0xf8,0xb3,0x2c,0x43,0xe7,0x22,0x52,0x56,0xc4,0xf8,0x37,
        0xa8,0x65,0x48,0xc9,0x2c,0xcc,0x35,0x48,0x08,0x05,0x98,0x7c,0xb7,0x0b,0xe1,0x7b
    };
    static const uint8_t exp_c2[32] = {
        0xae,0x4d,0x0c,0x95,0xaf,0x6b,0x46,0xd3,0x2d,0x0a,0xdf,0xf9,0x28,0xf0,0x6d,0xd0,
        0x2a,0x30,0x3f,0x8e,0xf3,0xc2,0x51,0xdf,0xd6,0xe2,0xd8,0x5a,0x95,0x47,0x4c,0x43
    };
    pbkdf2_hmac_sha256("password", 8, "salt", 4, 1, kdf);
    CHECK("PBKDF2-HMAC-SHA256 c=1 vector", sha256_equal(kdf, exp_c1));
    pbkdf2_hmac_sha256("password", 8, "salt", 4, 2, kdf);
    CHECK("PBKDF2-HMAC-SHA256 c=2 vector", sha256_equal(kdf, exp_c2));

    /* Persistence export/import across reboot: always starts LOCKED, rejects corruption */
    uint8_t rec[PIN_REC_BYTES];
    CHECK("export enrolled PIN succeeds", pin_export(&pl, rec) == PIN_OK);
    struct pin_lock pl2;
    pin_init(&pl2);
    CHECK("import valid record starts LOCKED", pin_import(&pl2, rec, 5000) == PIN_OK && pl2.state == PIN_STATE_LOCKED);
    CHECK("imported record verifies PIN", pin_unlock(&pl2, 5100, "7777", 4) == PIN_OK && pl2.state == PIN_STATE_UNLOCKED);

    rec[20] ^= 0x01;
    struct pin_lock pl_bad;
    pin_init(&pl_bad);
    CHECK("corrupted record rejected fail-closed", pin_import(&pl_bad, rec, 5200) == PIN_E_FORMAT && pl_bad.state == PIN_STATE_UNSET);

    printf("pin_lock: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
