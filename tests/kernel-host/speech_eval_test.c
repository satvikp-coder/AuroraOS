/* tests/kernel-host/speech_eval_test.c - Recorded/synthetic speech accuracy & false-accept evaluation (Priority B). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <speech_assistant.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static int eval_lookup(uint32_t pid, uint64_t *gen, uint32_t *principal, uint32_t *rights)
{
    if (pid == BP_SHELL_PID) {
        *gen = 1;
        *principal = 0x7F000001u;
        *rights = BP_RIGHT_NAVIGATE | BP_RIGHT_TIMER | BP_RIGHT_SETTINGS;
        return 1;
    }
    return 0;
}

static void extract_vec(const int16_t *pcm, size_t n, struct skw_vec *out)
{
    struct sf_frame_features f;
    sf_extract_frame(pcm, n, &f);
    uint64_t tot = f.low_band_energy + f.high_band_energy;
    int8_t ratio = tot ? (int8_t)((f.low_band_energy * 127ULL) / tot) : 0;
    out->v[0] = f.energy_q8;
    out->v[1] = f.zcr_q8;
    out->v[2] = ratio;
    out->v[3] = 32;
}

int main(void)
{
    audio_vad_config_t vcfg;
    audio_vad_default_config(&vcfg);
    vcfg.hangover_frames = 0;

    audio_vad_state_t vst;
    audio_vad_init(&vst, &vcfg);

    /* Register 3 distinct command classes */
    int16_t base_home[320], base_notes[320], base_timer[320];
    for (size_t i = 0; i < 320; i++) base_home[i]  = ((i / 16) % 2 == 0) ? 3000 : -3000;
    for (size_t i = 0; i < 320; i++) base_notes[i] = ((i / 3) % 2 == 0) ? 3000 : -3000;
    for (size_t i = 0; i < 320; i++) base_timer[i] = (i % 3 == 1) ? -3000 : 3000;

    struct skw_table tbl;
    skw_table_init(&tbl);
    struct skw_vec vh, vn, vt;
    extract_vec(base_home, 320, &vh);
    extract_vec(base_notes, 320, &vn);
    extract_vec(base_timer, 320, &vt);
    skw_add_template(&tbl, SKW_CMD_HOME, &vh);
    skw_add_template(&tbl, SKW_CMD_OPEN_NOTES, &vn);
    skw_add_template(&tbl, SKW_CMD_CANCEL_TIMER, &vt);

    struct aura_caps caps = { .pages = 1, .timer = 1, .notes = 1, .calculator = 1, .files = 1 };
    struct bp_env env = {
        .caps = &caps, .lookup = eval_lookup,
        .nav_service = 1, .timer_service = 1, .review_path = 1, .passport = 1, .trail = 1
    };
    struct bp_replay rp;
    bp_replay_init(&rp);

    int16_t tts[TTS_MAX_SAMPLES];
    struct sa_result res;
    uint64_t nonce = 100;

    /* 1. Positive corpus: 3 commands x 4 amplitude/noise perturbations = 12 utterances */
    const int16_t *bases[3] = { base_home, base_notes, base_timer };
    const enum skw_cmd expected_cmds[3] = { SKW_CMD_HOME, SKW_CMD_OPEN_NOTES, SKW_CMD_CANCEL_TIMER };
    int tp = 0, total_pos = 12;
    for (int c = 0; c < 3; ++c) {
        for (int v = 0; v < 4; ++v) {
            int16_t utt[320];
            int scale_num = 8 + v; /* 0.8x to 1.1x amplitude */
            for (size_t i = 0; i < 320; ++i) {
                int32_t s = (bases[c][i] * scale_num) / 10;
                /* small deterministic perturbation */
                int32_t noise = ((int32_t)((i * 37u + (size_t)v * 13u) % 201u)) - 100;
                utt[i] = (int16_t)(s + noise);
            }
            audio_vad_init(&vst, &vcfg);
            int rc = sa_run_turn(&vst, &vcfg, &tbl, utt, 320, &env, &rp, BP_SHELL_PID, 1, nonce++, tts, TTS_MAX_SAMPLES, &res);
            if (rc == SA_OK && res.kw.cmd == expected_cmds[c] && res.broker.decision == BP_ALLOW_NOW)
                tp++;
        }
    }
    CHECK("positive corpus 12/12 recognized and authorized", tp == total_pos);

    /* 2. Negative/OOV corpus: silence, DC offset, impulses, and out-of-vocabulary tones -> 0 false accepts */
    int fp = 0, total_neg = 8;
    for (int n = 0; n < total_neg; ++n) {
        int16_t neg[320];
        for (size_t i = 0; i < 320; ++i) {
            if (n == 0) neg[i] = 0;                                        /* silence */
            else if (n == 1) neg[i] = 4000;                                /* pure DC (removed by DC filter) */
            else if (n == 2) neg[i] = (i == 100) ? 12000 : 0;              /* single impulse */
            else if (n == 3) neg[i] = ((i / 64) % 2 == 0) ? 200 : -200;    /* low-energy hum */
            else neg[i] = ((i / (size_t)(40 + n * 10)) % 2 == 0) ? 12000 : -12000; /* OOV high-energy frequency */
        }
        audio_vad_init(&vst, &vcfg);
        int rc = sa_run_turn(&vst, &vcfg, &tbl, neg, 320, &env, &rp, BP_SHELL_PID, 1, nonce++, tts, TTS_MAX_SAMPLES, &res);
        if (rc == SA_OK && res.broker.decision == BP_ALLOW_NOW)
            fp++;
    }
    CHECK("negative/OOV corpus 0/8 false accepts", fp == 0);

    printf("speech_eval: tp=%d/%d fp=%d/%d (%d passed, %d failed)\n", tp, total_pos, fp, total_neg, passed, failed);
    return failed ? 1 : 0;
}
