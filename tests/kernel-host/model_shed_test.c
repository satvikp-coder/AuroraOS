/*
 * tests/kernel-host/model_shed_test.c - Host tests for low-memory model
 * load shedding, unload callback invocation, and deterministic Aura fallback.
 */
#include <model_shed.h>
#include <apps.h>
#include <stdio.h>
#include <string.h>

static int pass_count = 0;
static int fail_count = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        pass_count++;
    } else {
        fail_count++;
        fprintf(stderr, "FAIL: %s\n", name);
    }
}

struct fake_weights {
    int unload_calls;
    uint8_t buf[64];
};

static void test_unload_cb(void *ctx)
{
    struct fake_weights *fw = (struct fake_weights *)ctx;
    fw->unload_calls++;
    memset(fw->buf, 0, sizeof(fw->buf));
}

int main(void)
{
    struct ms_tracker t;
    struct fake_weights fw;
    struct aura_caps caps = { 1, 1, 1, 1, 1, 1, 1, 1 };
    struct aura_intent intent;

    memset(&fw, 0xA5, sizeof(fw));
    fw.unload_calls = 0;

    /* 1. Initial state is unloaded with fallback active */
    ms_init(&t, 32ULL * 1024 * 1024, 16ULL * 1024 * 1024);
    check(t.state == MS_UNLOADED, "init state unloaded");
    check(t.fallback_active == 1, "init fallback active");
    check(t.resident_bytes == 0, "init zero resident bytes");

    /* 2. Reject load when free memory is below model + headroom */
    check(ms_try_load(&t, 64ULL * 1024 * 1024, 60ULL * 1024 * 1024) == MS_ELOWMEM,
          "reject load when headroom insufficient");
    check(t.state == MS_UNLOADED, "remains unloaded after failed load");
    check(t.fallback_active == 1, "fallback stays active after failed load");

    /* 3. Reject load when free - model < low_watermark */
    check(ms_try_load(&t, 80ULL * 1024 * 1024, 55ULL * 1024 * 1024) == MS_ELOWMEM,
          "reject load when remaining falls below low watermark");

    /* 4. Succeed load when memory is plentiful */
    check(ms_try_load(&t, 256ULL * 1024 * 1024, 64ULL * 1024 * 1024) == MS_OK,
          "load succeeds with sufficient memory");
    check(t.state == MS_RESIDENT, "state is resident");
    check(t.fallback_active == 0, "fallback inactive while resident");
    check(t.resident_bytes == 64ULL * 1024 * 1024, "resident bytes recorded");
    check(t.load_count == 1, "load count incremented");

    /* 5. Fallback routing refuses while model is resident */
    check(ms_route_fallback(&t, "open notes", 10, &caps, &intent) == MS_ESTATE,
          "fallback refused while resident");

    /* 6. Memory pressure check above watermark keeps model resident */
    check(ms_check_memory_pressure(&t, 40ULL * 1024 * 1024, test_unload_cb, &fw) == MS_OK,
          "pressure check above watermark ok");
    check(t.state == MS_RESIDENT && fw.unload_calls == 0,
          "no unload above watermark");

    /* 7. Memory pressure below watermark triggers unload + wipe + MS_SHED */
    check(ms_check_memory_pressure(&t, 20ULL * 1024 * 1024, test_unload_cb, &fw) == MS_OK,
          "pressure check below watermark triggers shed");
    check(t.state == MS_SHED, "state transitioned to MS_SHED");
    check(t.resident_bytes == 0, "resident bytes zeroed after shed");
    check(t.fallback_active == 1, "fallback activated after shed");
    check(t.shed_count == 1, "shed count incremented");
    check(fw.unload_calls == 1, "unload callback invoked once");
    check(fw.buf[0] == 0 && fw.buf[63] == 0, "weights buffer cleared");

    /* 8. Repeated pressure check does not double-unload */
    check(ms_check_memory_pressure(&t, 10ULL * 1024 * 1024, test_unload_cb, &fw) == MS_OK,
          "repeated pressure check safe");
    check(fw.unload_calls == 1 && t.shed_count == 1, "no double unload");

    /* 9. Deterministic Aura fallback routes Quick Commands when shed */
    check(ms_route_fallback(&t, "open notes", 10, &caps, &intent) == MS_OK,
          "fallback routes open notes");
    check(intent.kind == AK_OPEN_APP && intent.status == AS_ACTIONABLE && intent.target == APP_NOTES,
          "fallback open notes intent validated");

    check(ms_route_fallback(&t, "timer 5 minutes", 15, &caps, &intent) == MS_OK,
          "fallback routes timer 5 minutes");
    check(intent.kind == AK_TIMER && intent.status == AS_ACTIONABLE && intent.arg == 300,
          "fallback timer 5 minutes intent validated");
    check(t.fallback_count == 2, "fallback count tracked");

    /* 10. Reload succeeds once memory recovers */
    check(ms_try_load(&t, 256ULL * 1024 * 1024, 64ULL * 1024 * 1024) == MS_OK,
          "reload succeeds after memory recovery");
    check(t.state == MS_RESIDENT && t.fallback_active == 0 && t.load_count == 2,
          "reloaded state restored");

    /* 11. Explicit voluntary unload transitions to MS_UNLOADED without incrementing shed_count */
    check(ms_unload(&t, test_unload_cb, &fw, 0) == MS_OK, "voluntary unload ok");
    check(t.state == MS_UNLOADED && t.shed_count == 1 && fw.unload_calls == 2,
          "voluntary unload state");

    printf("model_shed: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
