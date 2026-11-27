/* tests/kernel-host/mic_indicator_test.c - host tests for kernel/include/mic_indicator.h */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <mic_indicator.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

int main(void)
{
    struct mic_indicator m;
    mic_ind_init(&m);

    CHECK("init: state is OFF", m.state == MIC_IND_OFF);
    CHECK("init: label is empty", strcmp(mic_ind_label(&m), "") == 0);
    CHECK("null label is empty", strcmp(mic_ind_label(0), "") == 0);

    /* Bad args */
    CHECK("null indicator rejected", mic_ind_begin(0, MIC_ACTOR_COMPOSITOR, 100, 1, 1) == MIC_E_ARG);
    CHECK("principal 0 rejected", mic_ind_begin(&m, MIC_ACTOR_COMPOSITOR, 0, 1, 1) == MIC_E_ARG);
    CHECK("pid 0 rejected", mic_ind_begin(&m, MIC_ACTOR_COMPOSITOR, 100, 0, 1) == MIC_E_ARG);
    CHECK("gen 0 rejected", mic_ind_begin(&m, MIC_ACTOR_COMPOSITOR, 100, 1, 0) == MIC_E_ARG);

    /* Client spoof attempts refused and counted */
    CHECK("client begin refused", mic_ind_begin(&m, MIC_ACTOR_CLIENT, 100, 1, 1) == MIC_E_ACTOR && m.state == MIC_IND_OFF);
    CHECK("spoof counted", m.denied_spoofs == 1);

    /* Compositor begin -> LISTENING */
    CHECK("compositor begin succeeds", mic_ind_begin(&m, MIC_ACTOR_COMPOSITOR, 100, 5, 2) == MIC_OK && m.state == MIC_IND_LISTENING);
    CHECK("label is Mic: Listening", strcmp(mic_ind_label(&m), "Mic: Listening") == 0);
    CHECK("busy while already listening", mic_ind_begin(&m, MIC_ACTOR_COMPOSITOR, 101, 6, 3) == MIC_E_BUSY);

    /* Client cannot stop, pause, or notify */
    CHECK("client pause refused", mic_ind_pause(&m, MIC_ACTOR_CLIENT) == MIC_E_ACTOR && m.state == MIC_IND_LISTENING);
    CHECK("client stop refused", mic_ind_stop(&m, MIC_ACTOR_CLIENT) == MIC_E_ACTOR && m.state == MIC_IND_LISTENING);
    CHECK("client notify refused", mic_ind_notify_frame(&m, MIC_ACTOR_CLIENT) == MIC_E_ACTOR);
    CHECK("spoofs incremented to 4", m.denied_spoofs == 4);

    /* Notify, pause, resume, stop */
    CHECK("notify frame increments", mic_ind_notify_frame(&m, MIC_ACTOR_COMPOSITOR) == MIC_OK && m.frames_notified == 1);
    CHECK("pause succeeds", mic_ind_pause(&m, MIC_ACTOR_COMPOSITOR) == MIC_OK && m.state == MIC_IND_PAUSED);
    CHECK("label is Mic: Paused", strcmp(mic_ind_label(&m), "Mic: Paused") == 0);
    CHECK("notify while paused refused", mic_ind_notify_frame(&m, MIC_ACTOR_COMPOSITOR) == MIC_E_STATE);
    CHECK("resume succeeds", mic_ind_resume(&m, MIC_ACTOR_COMPOSITOR) == MIC_OK && m.state == MIC_IND_LISTENING);
    CHECK("stop clears state and owner", mic_ind_stop(&m, MIC_ACTOR_COMPOSITOR) == MIC_OK && m.state == MIC_IND_OFF && m.owner_pid == 0);

    /* Owner gone: stale generation ignored, matching generation clears */
    CHECK("re-begin succeeds", mic_ind_begin(&m, MIC_ACTOR_COMPOSITOR, 200, 7, 10) == MIC_OK);
    CHECK("stale gen owner_gone ignored", mic_ind_owner_gone(&m, 7, 9) == 0 && m.state == MIC_IND_LISTENING);
    CHECK("other pid owner_gone ignored", mic_ind_owner_gone(&m, 8, 10) == 0 && m.state == MIC_IND_LISTENING);
    CHECK("matching owner_gone clears", mic_ind_owner_gone(&m, 7, 10) == 1 && m.state == MIC_IND_OFF && m.owner_principal == 0);

    /* Revoke principal */
    CHECK("begin for principal 300", mic_ind_begin(&m, MIC_ACTOR_COMPOSITOR, 300, 9, 1) == MIC_OK);
    CHECK("client revoke refused", mic_ind_revoke_principal(&m, MIC_ACTOR_CLIENT, 300) == MIC_E_ACTOR && m.state == MIC_IND_LISTENING);
    CHECK("other principal revoke no-op", mic_ind_revoke_principal(&m, MIC_ACTOR_COMPOSITOR, 301) == 0 && m.state == MIC_IND_LISTENING);
    CHECK("matching principal revoke sets REVOKED", mic_ind_revoke_principal(&m, MIC_ACTOR_COMPOSITOR, 300) == 1 && m.state == MIC_IND_REVOKED);
    CHECK("label is Mic: Revoked", strcmp(mic_ind_label(&m), "Mic: Revoked") == 0);
    CHECK("can begin again after REVOKED", mic_ind_begin(&m, MIC_ACTOR_COMPOSITOR, 302, 10, 1) == MIC_OK && m.state == MIC_IND_LISTENING);

    printf("mic_indicator: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
