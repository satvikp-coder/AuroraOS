/* tests/kernel-host/capture_lifecycle_test.c - host tests for kernel/include/capture_lifecycle.h */
#include <stdio.h>
#include <stdint.h>
#include <capture_lifecycle.h>
#include <virtio_snd.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static struct cap_owner owner(uint32_t principal)
{
    return (struct cap_owner){principal, 1};
}

int main(void)
{
    struct cap_session s;
    cap_session_init(&s);
    CHECK("init: state is CAP_IDLE", s.state == CAP_IDLE);
    CHECK("init: principal is 0", s.owner.principal == 0);

    /* Null / bad args */
    CHECK("null session rejected on authorize", cap_authorize(0, owner(100), AUDIO_CAP_RECORD, 0) == CAP_E_ARG);
    CHECK("principal 0 rejected on authorize", cap_authorize(&s, owner(0), AUDIO_CAP_RECORD, 0) == CAP_E_ARG);
    CHECK("principal NONE rejected on authorize", cap_authorize(&s, owner(0xFFFFFFFFu), AUDIO_CAP_RECORD, 0) == CAP_E_ARG);
    CHECK("missing AUDIO_CAP_RECORD rejected", cap_authorize(&s, owner(100), 0, 0) == CAP_E_CAP && s.state == CAP_IDLE);

    CHECK("playback-only grant cannot authorize capture",
          cap_authorize(&s, owner(100), AUDIO_CAP_PLAY, 0) == CAP_E_CAP && s.state == CAP_IDLE);

    /* Invalid transitions from IDLE */
    CHECK("IDLE -> prepare refused", cap_prepare(&s, owner(100), 4) == CAP_E_STATE);
    CHECK("IDLE -> start refused", cap_start(&s, owner(100)) == CAP_E_STATE);
    CHECK("IDLE -> stop refused", cap_stop(&s, owner(100)) == CAP_E_STATE);
    CHECK("IDLE -> release refused", cap_release(&s, owner(100)) == CAP_E_STATE);
    CHECK("IDLE -> record refused", cap_record_bytes(&s, owner(100), 64) == CAP_E_STATE);

    /* Valid full lifecycle: IDLE -> AUTHORIZED -> PREPARED -> RUNNING -> STOPPED -> RUNNING -> STOPPED -> IDLE */
    CHECK("IDLE -> AUTHORIZED succeeds", cap_authorize(&s, owner(100), AUDIO_CAP_RECORD, 1) == CAP_OK && s.state == CAP_AUTHORIZED);
    CHECK("double authorize refused", cap_authorize(&s, owner(100), AUDIO_CAP_RECORD, 1) == CAP_E_STATE);
    CHECK("AUTHORIZED -> start refused", cap_start(&s, owner(100)) == CAP_E_STATE);
    CHECK("AUTHORIZED -> stop refused", cap_stop(&s, owner(100)) == CAP_E_STATE);
    CHECK("AUTHORIZED -> release by a different owner refused", cap_release(&s, owner(200)) == CAP_E_OWNER);
    CHECK("AUTHORIZED -> IDLE cancellation succeeds", cap_release(&s, owner(100)) == CAP_OK && s.state == CAP_IDLE);
    CHECK("cancelled session can be authorized again", cap_authorize(&s, owner(100), AUDIO_CAP_RECORD, 1) == CAP_OK);
    CHECK("AUTHORIZED -> prepare wrong owner refused", cap_prepare(&s, owner(200), 4) == CAP_E_OWNER);
    CHECK("AUTHORIZED -> prepare 0 buffers refused", cap_prepare(&s, owner(100), 0) == CAP_E_BOUNDS);
    CHECK("AUTHORIZED -> prepare over CAP_MAX_BUFFERS refused", cap_prepare(&s, owner(100), CAP_MAX_BUFFERS + 1) == CAP_E_BOUNDS);
    CHECK("AUTHORIZED -> PREPARED succeeds", cap_prepare(&s, owner(100), 4) == CAP_OK && s.state == CAP_PREPARED && s.buffer_count == 4);

    CHECK("PREPARED -> prepare again refused", cap_prepare(&s, owner(100), 4) == CAP_E_STATE);
    CHECK("PREPARED -> stop refused", cap_stop(&s, owner(100)) == CAP_E_STATE);
    CHECK("PREPARED -> start wrong owner refused", cap_start(&s, owner(200)) == CAP_E_OWNER);
    CHECK("PREPARED -> RUNNING succeeds", cap_start(&s, owner(100)) == CAP_OK && s.state == CAP_RUNNING);

    CHECK("RUNNING -> start again (double start) refused", cap_start(&s, owner(100)) == CAP_E_STATE);
    CHECK("RUNNING -> release refused", cap_release(&s, owner(100)) == CAP_E_STATE);
    CHECK("RUNNING -> record wrong owner refused", cap_record_bytes(&s, owner(200), 512) == CAP_E_OWNER);
    CHECK("RUNNING -> record 512 bytes succeeds", cap_record_bytes(&s, owner(100), 512) == CAP_OK && s.bytes_captured == 512);
    CHECK("RUNNING -> record past CAP_MAX_BYTES refused", cap_record_bytes(&s, owner(100), CAP_MAX_BYTES) == CAP_E_BOUNDS && s.bytes_captured == 512);
    CHECK("RUNNING -> stop wrong owner refused", cap_stop(&s, owner(200)) == CAP_E_OWNER);
    CHECK("RUNNING -> STOPPED succeeds", cap_stop(&s, owner(100)) == CAP_OK && s.state == CAP_STOPPED && s.buffer_count == 4);

    CHECK("STOPPED -> stop again (double stop) refused", cap_stop(&s, owner(100)) == CAP_E_STATE);
    CHECK("STOPPED -> record refused", cap_record_bytes(&s, owner(100), 64) == CAP_E_STATE);
    CHECK("STOPPED -> RUNNING resume succeeds", cap_start(&s, owner(100)) == CAP_OK && s.state == CAP_RUNNING);
    CHECK("RUNNING -> STOPPED again succeeds", cap_stop(&s, owner(100)) == CAP_OK && s.state == CAP_STOPPED);
    CHECK("STOPPED -> release wrong owner refused", cap_release(&s, owner(200)) == CAP_E_OWNER);
    CHECK("STOPPED -> IDLE release succeeds", cap_release(&s, owner(100)) == CAP_OK && s.state == CAP_IDLE && s.owner.principal == 0);

    /* PREPARED -> IDLE release without starting */
    CHECK("re-authorize after release succeeds", cap_authorize(&s, owner(101), AUDIO_CAP_RECORD, 2) == CAP_OK);
    CHECK("prepare succeeds", cap_prepare(&s, owner(101), 2) == CAP_OK);
    CHECK("PREPARED -> IDLE release succeeds", cap_release(&s, owner(101)) == CAP_OK && s.state == CAP_IDLE);

    /* Revocation from every state and reuse after release */
    CHECK("revoke from IDLE moves to REVOKED", cap_revoke(&s) == CAP_OK && s.state == CAP_REVOKED);
    cap_session_init(&s);
    cap_authorize(&s, owner(102), AUDIO_CAP_RECORD, 0);
    CHECK("revoke from AUTHORIZED moves to REVOKED", cap_revoke(&s) == CAP_OK && s.state == CAP_REVOKED && s.caps == 0);
    CHECK("REVOKED -> prepare refused", cap_prepare(&s, owner(102), 2) == CAP_E_STATE);
    CHECK("REVOKED -> start refused", cap_start(&s, owner(102)) == CAP_E_STATE);
    CHECK("REVOKED -> release by owner moves to IDLE", cap_release(&s, owner(102)) == CAP_OK && s.state == CAP_IDLE);
    CHECK("re-authorize after REVOKED->release succeeds", cap_authorize(&s, owner(103), AUDIO_CAP_RECORD, 0) == CAP_OK);
    cap_prepare(&s, owner(103), 2);
    cap_start(&s, owner(103));
    CHECK("revoke from RUNNING moves to REVOKED and clears buffers", cap_revoke(&s) == CAP_OK && s.state == CAP_REVOKED && s.buffer_count == 0);
    CHECK("REVOKED -> release cleans up", cap_release(&s, owner(103)) == CAP_OK && s.state == CAP_IDLE);

    /* Concurrent sessions via cap_table */
    struct cap_table tbl;
    cap_table_init(&tbl);
    struct cap_handle handle = {0};
    for (uint32_t i = 0; i < CAP_MAX_SESSIONS; i++) {
        CHECK("table open fills slot", cap_table_open(&tbl, owner(500 + i), AUDIO_CAP_RECORD, i, &handle) == CAP_OK && handle.slot == i);
    }
    CHECK("table full when CAP_MAX_SESSIONS active", cap_table_open(&tbl, owner(600), AUDIO_CAP_RECORD, 0, &handle) == CAP_E_FULL);
    CHECK("revoke principal 501 revokes only 1 session", cap_table_revoke_principal(&tbl, 501) == 1 && tbl.sessions[1].state == CAP_REVOKED && tbl.sessions[0].state == CAP_AUTHORIZED);

    CHECK("repeated revoke reports no additional sessions",
          cap_table_revoke_principal(&tbl, 501) == 0);
    CHECK("revoked slot remains reserved until release",
          cap_table_open(&tbl, owner(501), AUDIO_CAP_RECORD, 1, &handle) == CAP_E_FULL);
    CHECK("different principal cannot release revoked slot",
          cap_release(&tbl.sessions[1], owner(600)) == CAP_E_OWNER && tbl.sessions[1].state == CAP_REVOKED);

    struct cap_owner old = {501, 1}, fresh = {501, 2};
    struct cap_handle old_handle = {1, tbl.serial[1]};
    CHECK("new generation cannot release old revoked session",
          cap_release(cap_table_session(&tbl, old_handle), fresh) == CAP_E_OWNER);
    CHECK("original owner releases revoked slot",
          cap_release(cap_table_session(&tbl, old_handle), old) == CAP_OK);
    CHECK("released handle invalid", !cap_table_session(&tbl, old_handle));
    CHECK("slot reopened for fresh generation",
          cap_table_open(&tbl, fresh, AUDIO_CAP_RECORD, 1, &handle) == CAP_OK &&
          handle.slot == old_handle.slot && handle.serial != old_handle.serial);
    struct cap_session *live = cap_table_session(&tbl, handle);
    CHECK("stale handle cannot resolve reopened slot", live && !cap_table_session(&tbl, old_handle));
    CHECK("old generation cannot prepare or start", cap_prepare(live, old, 2) == CAP_E_OWNER &&
          cap_prepare(live, fresh, 2) == CAP_OK && cap_start(live, old) == CAP_E_OWNER);
    CHECK("fresh generation can start", cap_start(live, fresh) == CAP_OK);
    CHECK("old generation cannot consume bytes or stop", cap_record_bytes(live, old, 64) == CAP_E_OWNER &&
          live->bytes_captured == 0 && cap_stop(live, old) == CAP_E_OWNER);
    CHECK("old generation cannot release after stop", cap_stop(live, fresh) == CAP_OK &&
          cap_release(live, old) == CAP_E_OWNER && cap_release(live, fresh) == CAP_OK);
    old_handle = handle;
    CHECK("same generation reopening also invalidates previous handle",
          cap_table_open(&tbl, fresh, AUDIO_CAP_RECORD, 1, &handle) == CAP_OK &&
          !cap_table_session(&tbl, old_handle));
    cap_table_init(&tbl);
    tbl.serial[0] = UINT64_MAX;
    CHECK("exhausted slot serial never wraps",
          cap_table_open(&tbl, fresh, AUDIO_CAP_RECORD, 1, &handle) == CAP_OK && handle.slot == 1);
    fresh.generation = 0;
    CHECK("zero generation refused", cap_table_open(&tbl, fresh, AUDIO_CAP_RECORD, 1, &handle) == CAP_E_ARG);

    printf("capture_lifecycle: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
