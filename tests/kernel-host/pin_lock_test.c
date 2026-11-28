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

    printf("pin_lock: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
