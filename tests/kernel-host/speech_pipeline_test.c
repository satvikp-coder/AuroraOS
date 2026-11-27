/* tests/kernel-host/speech_pipeline_test.c - host tests for kernel/include/speech_pipeline.h */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <speech_pipeline.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static int fake_lookup(uint32_t pid, uint64_t *gen, uint32_t *principal, uint32_t *rights)
{
    if (pid == BP_SHELL_PID) {
        *gen = 1;
        *principal = 0x7F000001u;
        *rights = BP_RIGHT_NAVIGATE | BP_RIGHT_TIMER | BP_RIGHT_SETTINGS;
        return 1;
    }
    if (pid == 42) {
        *gen = 3;
        *principal = 602;
        *rights = BP_RIGHT_NAVIGATE;
        return 1;
    }
    return 0;
}

int main(void)
{
    audio_vad_config_t vcfg;
    audio_vad_default_config(&vcfg);
    vcfg.hangover_frames = 0; /* immediate silence test */

    audio_vad_state_t vst;
    audio_vad_init(&vst, &vcfg);

    struct aura_caps caps = { .pages = 1, .timer = 1, .notes = 1, .calculator = 1, .files = 1 };
    struct bp_env env = {
        .caps = &caps,
        .lookup = fake_lookup,
        .nav_service = 1,
        .timer_service = 1,
        .review_path = 1,
        .passport = 1,
        .trail = 1
    };
    struct bp_replay rp;
    bp_replay_init(&rp);

    int16_t pcm[320];
    struct sp_pipeline_out out;

    /* Bad args */
    CHECK("null out rejected", sp_process_pcm_to_broker(&vst, &vcfg, pcm, 320, &env, &rp, BP_SHELL_PID, 1, 1, 0) == SP_E_ARG);
    CHECK("null pcm rejected", sp_process_pcm_to_broker(&vst, &vcfg, 0, 320, &env, &rp, BP_SHELL_PID, 1, 1, &out) == SP_E_ARG);

    /* Silence stops at VAD */
    memset(pcm, 0, sizeof(pcm));
    CHECK("silence stops at VAD", sp_process_pcm_to_broker(&vst, &vcfg, pcm, 320, &env, &rp, BP_SHELL_PID, 1, 1, &out) == SP_E_SILENCE);
    CHECK("silence leaves intent kind AK_NONE", out.intent.kind == AK_NONE);

    /* Low ZCR (~62 permille) -> "open home" -> BP_ALLOW_NOW for shell */
    for (size_t i = 0; i < 320; i++) pcm[i] = ((i / 16) % 2 == 0) ? 2000 : -2000;
    CHECK("low ZCR routes to open home + BP_ALLOW_NOW",
          sp_process_pcm_to_broker(&vst, &vcfg, pcm, 320, &env, &rp, BP_SHELL_PID, 1, 10, &out) == SP_OK &&
          out.intent.kind == AK_OPEN_SCREEN && out.intent.target == ASC_HOME && out.broker.decision == BP_ALLOW_NOW);

    /* Mid ZCR (~333 permille, period 6 -> 3 pos, 3 neg) -> "open notes" -> BP_ALLOW_NOW for shell */
    for (size_t i = 0; i < 320; i++) pcm[i] = ((i / 3) % 2 == 0) ? 2000 : -2000;
    CHECK("mid ZCR routes to open notes + BP_ALLOW_NOW",
          sp_process_pcm_to_broker(&vst, &vcfg, pcm, 320, &env, &rp, BP_SHELL_PID, 1, 11, &out) == SP_OK &&
          out.intent.kind == AK_OPEN_APP && out.broker.decision == BP_ALLOW_NOW);

    /* Same mid ZCR from unprivileged EL0 pid 42 -> DENIED by Action Broker (no confused deputy) */
    CHECK("mid ZCR from EL0 pid 42 denied by broker",
          sp_process_pcm_to_broker(&vst, &vcfg, pcm, 320, &env, &rp, 42, 3, 1, &out) == SP_E_BROKER &&
          out.broker.decision == BP_DENY && out.broker.reason == BPR_NO_RIGHT);

    /* Replay nonce rejected */
    CHECK("replayed nonce denied by broker",
          sp_process_pcm_to_broker(&vst, &vcfg, pcm, 320, &env, &rp, BP_SHELL_PID, 1, 11, &out) == SP_E_BROKER &&
          out.broker.decision == BP_DENY && out.broker.reason == BPR_REPLAY);

    /* High ZCR (~1000 permille) -> out-of-band noise -> SP_E_NO_MATCH */
    for (size_t i = 0; i < 320; i++) pcm[i] = (i % 2 == 0) ? 2000 : -2000;
    CHECK("1000 permille noise rejected as NO_MATCH",
          sp_process_pcm_to_broker(&vst, &vcfg, pcm, 320, &env, &rp, BP_SHELL_PID, 1, 12, &out) == SP_E_NO_MATCH);

    /* High ZCR in [600..900] -> "cancel timer" */
    /* Pattern: +2000, -2000, +2000, +2000 (2 crossings per 4 samples = 500; let's do 3 crossings per 4: +,-,+,-,+,-,-,+ -> ~750) */
    for (size_t i = 0; i < 320; i++) pcm[i] = (i % 4 == 0) ? 2000 : (i % 4 == 1) ? -2000 : (i % 4 == 2) ? 2000 : 2000;
    /* Wait: i%4: 0(+), 1(-), 2(+), 3(+) -> crossings at 0->1, 1->2 (2 per 4 = 500). Let's do i%3: +,-,+ -> 2 crossings per 3 = 667 permille! */
    for (size_t i = 0; i < 320; i++) pcm[i] = (i % 3 == 1) ? -2000 : 2000;
    CHECK("667 permille routes to cancel timer + BP_ALLOW_NOW",
          sp_process_pcm_to_broker(&vst, &vcfg, pcm, 320, &env, &rp, BP_SHELL_PID, 1, 13, &out) == SP_OK &&
          out.intent.kind == AK_TIMER && out.intent.target == ATM_CANCEL && out.broker.decision == BP_ALLOW_NOW);

    printf("speech_pipeline: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
