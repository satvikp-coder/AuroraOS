/* tests/kernel-host/passport_enforce_test.c - host tests for passport_revoke_all, passport_count_grants, passport_has_any */
#include <stdio.h>
#include <stdint.h>
#include <passport.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

int main(void)
{
    struct passport p;
    struct pp_actor kern = { PP_ACTOR_KERNEL, 0, 1, 0 };
    struct pp_actor proc = { PP_ACTOR_PROCESS, 42, 3, 200 };

    passport_init(&p);
    CHECK("empty: count is 0", passport_count_grants(&p, 100) == 0);
    CHECK("empty: has_any is 0", passport_has_any(&p, 100) == 0);
    CHECK("null table count is 0", passport_count_grants(0, 100) == 0);
    CHECK("principal 0 count is 0", passport_count_grants(&p, 0) == 0);
    CHECK("revoke_all on empty principal returns 0", passport_revoke_all(&p, &kern, 100) == 0);

    /* Grant 3 permissions (mix of general and generation-bound) to principal 100 */
    CHECK("grant 1", passport_grant(&p, &kern, 100, 0, PP_NAVIGATE) == PP_OK);
    CHECK("grant 2", passport_grant(&p, &kern, 100, 5, PP_FILES_WRITE) == PP_OK);
    CHECK("grant 3", passport_grant(&p, &kern, 100, 5, PP_TIMER) == PP_OK);
    CHECK("count is 3", passport_count_grants(&p, 100) == 3);
    CHECK("has_any is 1", passport_has_any(&p, 100) == 1);

    /* Grant permissions to 2 other principals */
    CHECK("grant other A", passport_grant(&p, &kern, 101, 0, PP_NAVIGATE) == PP_OK);
    CHECK("grant other B", passport_grant(&p, &kern, 102, 7, PP_SETTINGS_PROPOSE) == PP_OK);

    /* Attempt revoke_all as PP_ACTOR_PROCESS -> refused and audited */
    uint32_t seq_before = p.seq;
    CHECK("process actor refused", passport_revoke_all(&p, &proc, 100) == PP_E_ACTOR);
    CHECK("process refusal audited", p.seq == seq_before + 1);
    CHECK("grants intact after process refusal", passport_count_grants(&p, 100) == 3);

    /* Bad args */
    CHECK("null table refused", passport_revoke_all(0, &kern, 100) == PP_E_ARG);
    CHECK("principal 0 refused", passport_revoke_all(&p, &kern, 0) == PP_E_PRINCIPAL);
    CHECK("principal NONE refused", passport_revoke_all(&p, &kern, 0xFFFFFFFFu) == PP_E_PRINCIPAL);

    /* Revoke all for 100 as KERNEL */
    seq_before = p.seq;
    CHECK("revoke_all returns 3", passport_revoke_all(&p, &kern, 100) == 3);
    CHECK("count after revoke_all is 0", passport_count_grants(&p, 100) == 0);
    CHECK("has_any after revoke_all is 0", passport_has_any(&p, 100) == 0);
    CHECK("check fails after revoke_all", passport_check(&p, 100, 0, PP_NAVIGATE) == 0);
    CHECK("gen check fails after revoke_all", passport_check(&p, 100, 5, PP_FILES_WRITE) == 0);
    CHECK("audit ring recorded 3 revocations", p.seq == seq_before + 3);

    /* Others untouched */
    CHECK("principal 101 untouched", passport_count_grants(&p, 101) == 1 && passport_has_any(&p, 101) == 1);
    CHECK("principal 102 untouched", passport_count_grants(&p, 102) == 1 && passport_check(&p, 102, 7, PP_SETTINGS_PROPOSE) == 1);

    /* Stress: fill table to PP_MAX_GRANTS across 4 principals (PP_PER_PRINCIPAL=8 each), revoke one at a time */
    passport_init(&p);
    for (uint32_t pr = 1; pr <= 4; pr++) {
        for (uint8_t perm = PP_NAVIGATE; perm < PP_COUNT; perm++) {
            passport_grant(&p, &kern, 200 + pr, 0, perm);
        }
        /* PP_COUNT is 8, so 7 perms (1..7) */
        passport_grant(&p, &kern, 200 + pr, 10, PP_NAVIGATE);
        CHECK("filled 8 grants per principal", passport_count_grants(&p, 200 + pr) == 8);
    }
    for (uint32_t pr = 1; pr <= 4; pr++) {
        CHECK("revoke_all drops all 8", passport_revoke_all(&p, &kern, 200 + pr) == 8);
        CHECK("count is 0 after stress revoke", passport_count_grants(&p, 200 + pr) == 0);
    }

    printf("passport_enforce: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
