/* tests/kernel-host/capture_lifecycle_test.c - host tests for kernel/include/capture_lifecycle.h */
#include <stdio.h>
#include <stdint.h>
#include <capture_lifecycle.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

int main(void)
{
    struct cap_session s;
    cap_session_init(&s);
    CHECK("init: state is CAP_IDLE", s.state == CAP_IDLE);
    CHECK("init: principal is 0", s.principal == 0);

    /* Null / bad args */
    CHECK("null session rejected on authorize", cap_authorize(0, 100, AUDIO_CAP_RECORD, 0) == CAP_E_ARG);
    CHECK("principal 0 rejected on authorize", cap_authorize(&s, 0, AUDIO_CAP_RECORD, 0) == CAP_E_ARG);
    CHECK("principal NONE rejected on authorize", cap_authorize(&s, 0xFFFFFFFFu, AUDIO_CAP_RECORD, 0) == CAP_E_ARG);
    CHECK("missing AUDIO_CAP_RECORD rejected", cap_authorize(&s, 100, 0, 0) == CAP_E_CAP && s.state == CAP_IDLE);

    /* Invalid transitions from IDLE */
    CHECK("IDLE -> prepare refused", cap_prepare(&s, 100, 4) == CAP_E_STATE);
    CHECK("IDLE -> start refused", cap_start(&s, 100) == CAP_E_STATE);
    CHECK("IDLE -> stop refused", cap_stop(&s, 100) == CAP_E_STATE);
    CHECK("IDLE -> release refused", cap_release(&s, 100) == CAP_E_STATE);
    CHECK("IDLE -> record refused", cap_record_bytes(&s, 100, 64) == CAP_E_STATE);

    /* Valid full lifecycle: IDLE -> AUTHORIZED -> PREPARED -> RUNNING -> STOPPED -> RUNNING -> STOPPED -> IDLE */
    CHECK("IDLE -> AUTHORIZED succeeds", cap_authorize(&s, 100, AUDIO_CAP_RECORD, 1) == CAP_OK && s.state == CAP_AUTHORIZED);
    CHECK("double authorize refused", cap_authorize(&s, 100, AUDIO_CAP_RECORD, 1) == CAP_E_STATE);
    CHECK("AUTHORIZED -> start refused", cap_start(&s, 100) == CAP_E_STATE);
    CHECK("AUTHORIZED -> stop refused", cap_stop(&s, 100) == CAP_E_STATE);
    CHECK("AUTHORIZED -> release refused", cap_release(&s, 100) == CAP_E_STATE);
    CHECK("AUTHORIZED -> prepare wrong owner refused", cap_prepare(&s, 200, 4) == CAP_E_OWNER);
    CHECK("AUTHORIZED -> prepare 0 buffers refused", cap_prepare(&s, 100, 0) == CAP_E_BOUNDS);
    CHECK("AUTHORIZED -> prepare over CAP_MAX_BUFFERS refused", cap_prepare(&s, 100, CAP_MAX_BUFFERS + 1) == CAP_E_BOUNDS);
    CHECK("AUTHORIZED -> PREPARED succeeds", cap_prepare(&s, 100, 4) == CAP_OK && s.state == CAP_PREPARED && s.buffer_count == 4);

    CHECK("PREPARED -> prepare again refused", cap_prepare(&s, 100, 4) == CAP_E_STATE);
    CHECK("PREPARED -> stop refused", cap_stop(&s, 100) == CAP_E_STATE);
    CHECK("PREPARED -> start wrong owner refused", cap_start(&s, 200) == CAP_E_OWNER);
    CHECK("PREPARED -> RUNNING succeeds", cap_start(&s, 100) == CAP_OK && s.state == CAP_RUNNING);

    CHECK("RUNNING -> start again (double start) refused", cap_start(&s, 100) == CAP_E_STATE);
    CHECK("RUNNING -> release refused", cap_release(&s, 100) == CAP_E_STATE);
    CHECK("RUNNING -> record wrong owner refused", cap_record_bytes(&s, 200, 512) == CAP_E_OWNER);
    CHECK("RUNNING -> record 512 bytes succeeds", cap_record_bytes(&s, 100, 512) == CAP_OK && s.bytes_captured == 512);
    CHECK("RUNNING -> record past CAP_MAX_BYTES refused", cap_record_bytes(&s, 100, CAP_MAX_BYTES) == CAP_E_BOUNDS && s.bytes_captured == 512);
    CHECK("RUNNING -> stop wrong owner refused", cap_stop(&s, 200) == CAP_E_OWNER);
    CHECK("RUNNING -> STOPPED succeeds", cap_stop(&s, 100) == CAP_OK && s.state == CAP_STOPPED && s.buffer_count == 4);

    CHECK("STOPPED -> stop again (double stop) refused", cap_stop(&s, 100) == CAP_E_STATE);
    CHECK("STOPPED -> record refused", cap_record_bytes(&s, 100, 64) == CAP_E_STATE);
    CHECK("STOPPED -> RUNNING resume succeeds", cap_start(&s, 100) == CAP_OK && s.state == CAP_RUNNING);
    CHECK("RUNNING -> STOPPED again succeeds", cap_stop(&s, 100) == CAP_OK && s.state == CAP_STOPPED);
    CHECK("STOPPED -> release wrong owner refused", cap_release(&s, 200) == CAP_E_OWNER);
    CHECK("STOPPED -> IDLE release succeeds", cap_release(&s, 100) == CAP_OK && s.state == CAP_IDLE && s.principal == 0);

    /* PREPARED -> IDLE release without starting */
    CHECK("re-authorize after release succeeds", cap_authorize(&s, 101, AUDIO_CAP_RECORD, 2) == CAP_OK);
    CHECK("prepare succeeds", cap_prepare(&s, 101, 2) == CAP_OK);
    CHECK("PREPARED -> IDLE release succeeds", cap_release(&s, 101) == CAP_OK && s.state == CAP_IDLE);

    /* Revocation from every state and reuse after release */
    CHECK("revoke from IDLE moves to REVOKED", cap_revoke(&s) == CAP_OK && s.state == CAP_REVOKED);
    cap_session_init(&s);
    cap_authorize(&s, 102, AUDIO_CAP_RECORD, 0);
    CHECK("revoke from AUTHORIZED moves to REVOKED", cap_revoke(&s) == CAP_OK && s.state == CAP_REVOKED && s.caps == 0);
    CHECK("REVOKED -> prepare refused", cap_prepare(&s, 102, 2) == CAP_E_STATE);
    CHECK("REVOKED -> start refused", cap_start(&s, 102) == CAP_E_STATE);
    CHECK("REVOKED -> release by owner moves to IDLE", cap_release(&s, 102) == CAP_OK && s.state == CAP_IDLE);
    CHECK("re-authorize after REVOKED->release succeeds", cap_authorize(&s, 103, AUDIO_CAP_RECORD, 0) == CAP_OK);
    cap_prepare(&s, 103, 2);
    cap_start(&s, 103);
    CHECK("revoke from RUNNING moves to REVOKED and clears buffers", cap_revoke(&s) == CAP_OK && s.state == CAP_REVOKED && s.buffer_count == 0);
    CHECK("REVOKED -> release cleans up", cap_release(&s, 103) == CAP_OK && s.state == CAP_IDLE);

    /* Concurrent sessions via cap_table */
    struct cap_table tbl;
    cap_table_init(&tbl);
    size_t slot = 99;
    for (uint32_t i = 0; i < CAP_MAX_SESSIONS; i++) {
        CHECK("table open fills slot", cap_table_open(&tbl, 500 + i, AUDIO_CAP_RECORD, i, &slot) == CAP_OK && slot == i);
    }
    CHECK("table full when CAP_MAX_SESSIONS active", cap_table_open(&tbl, 600, AUDIO_CAP_RECORD, 0, &slot) == CAP_E_FULL);
    CHECK("revoke principal 501 revokes only 1 session", cap_table_revoke_principal(&tbl, 501) == 1 && tbl.sessions[1].state == CAP_REVOKED && tbl.sessions[0].state == CAP_AUTHORIZED);

    printf("capture_lifecycle: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
