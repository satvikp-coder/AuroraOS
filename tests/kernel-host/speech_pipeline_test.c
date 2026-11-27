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

/* Synthetic recognizer result to exercise command routing without a speech model. */
static enum skw_cmd fixture_command;
static unsigned fixture_calls;
static enum skw_cmd fixture_recognize(const int16_t *pcm, size_t count)
{
    fixture_calls++;
    return pcm && count == 320 ? fixture_command : SKW_CMD_NONE;
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

    fixture_command = SKW_CMD_HOME;
    CHECK("silence never reaches recognizer",
          sp_process_with_recognizer(fixture_recognize, &vst, &vcfg, pcm, 320, &env, &rp,
                                     BP_SHELL_PID, 1, 10, &out) == SP_E_SILENCE && fixture_calls == 0);

    /* Active tones covering each former command band must leave the nonce unused. */
    const unsigned periods[] = {32, 6, 3};
    for (size_t band = 0; band < sizeof(periods) / sizeof(periods[0]); band++) {
        unsigned period = periods[band];
        for (size_t i = 0; i < 320; i++)
            pcm[i] = i % period < period / 2 ? 2000 : -2000;
        CHECK("active tone alone cannot propose a command",
              sp_process_pcm_to_broker(&vst, &vcfg, pcm, 320, &env, &rp,
                                       BP_SHELL_PID, 1, 10, &out) == SP_E_NO_MATCH &&
              out.stage_status == SP_E_NO_MATCH && !out.matched_phrase &&
              out.intent.kind == AK_NONE && out.broker.decision == BP_DENY);
    }

    fixture_command = SKW_CMD_NONE;
    CHECK("unknown speech stops before broker",
          sp_process_with_recognizer(fixture_recognize, &vst, &vcfg, pcm, 320, &env, &rp,
                                     BP_SHELL_PID, 1, 10, &out) == SP_E_NO_MATCH);
    fixture_command = SKW_CMD_COUNT;
    CHECK("invalid recognizer output stops before broker",
          sp_process_with_recognizer(fixture_recognize, &vst, &vcfg, pcm, 320, &env, &rp,
                                     BP_SHELL_PID, 1, 10, &out) == SP_E_NO_MATCH);
    fixture_command = SKW_CMD_HOME;
    CHECK("synthetic home result reaches broker",
          sp_process_with_recognizer(fixture_recognize, &vst, &vcfg, pcm, 320, &env, &rp,
                                     BP_SHELL_PID, 1, 10, &out) == SP_OK &&
          out.intent.kind == AK_OPEN_SCREEN && out.intent.target == ASC_HOME);
    fixture_command = SKW_CMD_OPEN_NOTES;
    CHECK("synthetic notes result reaches broker",
          sp_process_with_recognizer(fixture_recognize, &vst, &vcfg, pcm, 320, &env, &rp,
                                     BP_SHELL_PID, 1, 11, &out) == SP_OK && out.intent.kind == AK_OPEN_APP);
    CHECK("EL0 notes request denied",
          sp_process_with_recognizer(fixture_recognize, &vst, &vcfg, pcm, 320, &env, &rp,
                                     42, 3, 1, &out) == SP_E_BROKER && out.broker.reason == BPR_NO_RIGHT);
    CHECK("replayed nonce denied",
          sp_process_with_recognizer(fixture_recognize, &vst, &vcfg, pcm, 320, &env, &rp,
                                     BP_SHELL_PID, 1, 11, &out) == SP_E_BROKER && out.broker.reason == BPR_REPLAY);
    fixture_command = SKW_CMD_CANCEL_TIMER;
    CHECK("synthetic cancel result reaches broker",
          sp_process_with_recognizer(fixture_recognize, &vst, &vcfg, pcm, 320, &env, &rp,
                                     BP_SHELL_PID, 1, 12, &out) == SP_OK &&
          out.intent.kind == AK_TIMER && out.intent.target == ATM_CANCEL);

    printf("speech_pipeline: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
