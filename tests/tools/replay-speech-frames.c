/* Host replay of the fixed synthetic template set in PR #254. Input is native S16 mono at 16 kHz. */
#include <stdio.h>
#include <stdint.h>
#include <speech_assistant.h>


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

    int16_t frame[320];
    unsigned turns = 0;
    unsigned matches[SKW_CMD_COUNT] = {0};
    size_t n;
    while ((n = fread(frame, sizeof(*frame), 320, stdin)) > 0) {
        for (size_t i = n; i < 320; ++i) frame[i] = 0;
        int rc = sa_run_turn(&vst, &vcfg, &tbl, frame, 320, &env, &rp, BP_SHELL_PID, 1,
                             nonce++, tts, TTS_MAX_SAMPLES, &res);
        if (rc == SA_E_ARG || rc == SA_E_TTS) return 1;
        if (rc == SA_OK && res.broker.decision == BP_ALLOW_NOW) matches[res.kw.cmd]++;
        turns++;
    }
    if (ferror(stdin) || turns == 0) return 1;
    printf("%u\t%u\t%u\t%u\t%u\n", SKW_MAX_DIST, turns, matches[SKW_CMD_HOME],
           matches[SKW_CMD_OPEN_NOTES], matches[SKW_CMD_CANCEL_TIMER]);
    return 0;
}
