/* tests/kernel-host/speech_assistant_test.c - host tests for kernel/include/speech_assistant.h */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <speech_assistant.h>

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
    vcfg.hangover_frames = 0;

    audio_vad_state_t vst;
    audio_vad_init(&vst, &vcfg);

    /* Extract reference features from 3 distinct synthetic waveforms and register as keyword templates */
    int16_t pcm_home[320], pcm_notes[320], pcm_timer[320], pcm_silence[320];
    memset(pcm_silence, 0, sizeof(pcm_silence));
    for (size_t i = 0; i < 320; i++) pcm_home[i]  = ((i / 16) % 2 == 0) ? 3000 : -3000;
    for (size_t i = 0; i < 320; i++) pcm_notes[i] = ((i / 3) % 2 == 0) ? 3000 : -3000;
    for (size_t i = 0; i < 320; i++) pcm_timer[i] = (i % 3 == 1) ? -3000 : 3000;

    struct sf_frame_features fh, fn, ft;
    sf_extract_frame(pcm_home, 320, &fh);
    sf_extract_frame(pcm_notes, 320, &fn);
    sf_extract_frame(pcm_timer, 320, &ft);

    struct skw_table tbl;
    skw_table_init(&tbl);
    struct skw_vec vh = { { fh.energy_q8, fh.zcr_q8, (int8_t)((fh.low_band_energy * 127ULL) / (fh.low_band_energy + fh.high_band_energy)), 32 } };
    struct skw_vec vn = { { fn.energy_q8, fn.zcr_q8, (int8_t)((fn.low_band_energy * 127ULL) / (fn.low_band_energy + fn.high_band_energy)), 32 } };
    struct skw_vec vt = { { ft.energy_q8, ft.zcr_q8, (int8_t)((ft.low_band_energy * 127ULL) / (ft.low_band_energy + ft.high_band_energy)), 32 } };
    skw_add_template(&tbl, SKW_CMD_HOME, &vh);
    skw_add_template(&tbl, SKW_CMD_OPEN_NOTES, &vn);
    skw_add_template(&tbl, SKW_CMD_CANCEL_TIMER, &vt);

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

    int16_t tts[TTS_MAX_SAMPLES];
    struct sa_result res;

    /* Null out rejected */
    CHECK("null res rejected", sa_run_turn(&vst, &vcfg, &tbl, pcm_home, 320, &env, &rp, BP_SHELL_PID, 1, 1, tts, TTS_MAX_SAMPLES, 0) == SA_E_ARG);

    /* Silence stops at VAD, 0 TTS samples */
    CHECK("silence stops at VAD", sa_run_turn(&vst, &vcfg, &tbl, pcm_silence, 320, &env, &rp, BP_SHELL_PID, 1, 1, tts, TTS_MAX_SAMPLES, &res) == SA_SILENCE && res.tts_samples == 0);

    /* Shell HOME -> BP_ALLOW_NOW + "Home" TTS samples */
    CHECK("shell HOME -> SA_OK + TTS Home",
          sa_run_turn(&vst, &vcfg, &tbl, pcm_home, 320, &env, &rp, BP_SHELL_PID, 1, 10, tts, TTS_MAX_SAMPLES, &res) == SA_OK &&
          res.intent.kind == AK_OPEN_SCREEN && res.broker.decision == BP_ALLOW_NOW &&
          strcmp(res.spoken_cue, "Home") == 0 && res.tts_samples == 4u * TTS_SAMPLES_PER_CHAR);

    /* Shell OPEN_NOTES -> BP_ALLOW_NOW + "Notes" TTS samples */
    CHECK("shell OPEN_NOTES -> SA_OK + TTS Notes",
          sa_run_turn(&vst, &vcfg, &tbl, pcm_notes, 320, &env, &rp, BP_SHELL_PID, 1, 11, tts, TTS_MAX_SAMPLES, &res) == SA_OK &&
          res.intent.kind == AK_OPEN_APP && strcmp(res.spoken_cue, "Notes") == 0 && res.tts_samples == 5u * TTS_SAMPLES_PER_CHAR);

    /* Shell CANCEL_TIMER -> BP_ALLOW_NOW + "Cancel" TTS samples */
    CHECK("shell CANCEL_TIMER -> SA_OK + TTS Cancel",
          sa_run_turn(&vst, &vcfg, &tbl, pcm_timer, 320, &env, &rp, BP_SHELL_PID, 1, 12, tts, TTS_MAX_SAMPLES, &res) == SA_OK &&
          res.intent.kind == AK_TIMER && strcmp(res.spoken_cue, "Cancel") == 0 && res.tts_samples == 6u * TTS_SAMPLES_PER_CHAR);

    /* Unprivileged EL0 pid 42 OPEN_NOTES -> DENIED + "Denied" TTS cue */
    CHECK("unprivileged EL0 OPEN_NOTES -> SA_DENIED + TTS Denied",
          sa_run_turn(&vst, &vcfg, &tbl, pcm_notes, 320, &env, &rp, 42, 3, 1, tts, TTS_MAX_SAMPLES, &res) == SA_DENIED &&
          res.broker.decision == BP_DENY && strcmp(res.spoken_cue, "Denied") == 0 && res.tts_samples == 6u * TTS_SAMPLES_PER_CHAR);

    /* Replay nonce -> SA_DENIED + "Denied" TTS cue */
    CHECK("replay nonce -> SA_DENIED",
          sa_run_turn(&vst, &vcfg, &tbl, pcm_home, 320, &env, &rp, BP_SHELL_PID, 1, 10, tts, TTS_MAX_SAMPLES, &res) == SA_DENIED &&
          res.broker.reason == BPR_REPLAY);

    printf("speech_assistant: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
