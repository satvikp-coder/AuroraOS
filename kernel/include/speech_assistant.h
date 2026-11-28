/*
 * kernel/include/speech_assistant.h - End-to-end offline speech-to-Aura-to-TTS loop (Issue #13).
 *
 * Pure C, freestanding, no allocation, no floating point, no libc, no globals.
 * Connects:
 *   PCM16 frame -> audio_vad_process_frame() -> sf_extract_frame() -> skw_recognize()
 *   -> aura_parse() -> aura_validate() -> bp_decide() -> tts_synthesize()
 *
 * Fail-closed:
 *   - Silence stops at VAD (0 TTS samples produced).
 *   - Unrecognized or ambiguous speech stops before Aura (0 TTS samples produced).
 *   - Denied or replayed requests never report success; they produce "Denied" cue only.
 */
#ifndef AURORA_SPEECH_ASSISTANT_H
#define AURORA_SPEECH_ASSISTANT_H

#include <stddef.h>
#include <stdint.h>
#include <audio_vad.h>
#include <speech_feat.h>
#include <speech_kw.h>
#include <aura_cmd.h>
#include <broker_policy.h>
#include <tts_synth.h>

enum sa_status {
    SA_OK = 0,
    SA_E_ARG = -1,
    SA_SILENCE = -2,
    SA_NO_MATCH = -3,
    SA_E_INTENT = -4,
    SA_DENIED = -5,
    SA_E_TTS = -6
};

struct sa_result {
    int32_t status;
    audio_vad_result_t vad;
    struct sf_frame_features feat;
    struct skw_match kw;
    struct aura_intent intent;
    struct bp_result broker;
    size_t tts_samples;
    const char *spoken_cue;
};

static inline const char *sa_cmd_to_phrase(uint8_t cmd, const char **out_cue)
{
    switch (cmd) {
    case SKW_CMD_HOME:
        if (out_cue) *out_cue = "Home";
        return "open home";
    case SKW_CMD_OPEN_NOTES:
        if (out_cue) *out_cue = "Notes";
        return "open notes";
    case SKW_CMD_CANCEL_TIMER:
        if (out_cue) *out_cue = "Cancel";
        return "cancel timer";
    default:
        if (out_cue) *out_cue = 0;
        return 0;
    }
}

static inline size_t sa_cstr_len(const char *s)
{
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static inline uint8_t sa_clamp_q7(int32_t v)
{
    if (v < 0) return 0;
    if (v > 127) return 127;
    return (uint8_t)v;
}

static inline int sa_run_turn(audio_vad_state_t *vad_state,
                              const audio_vad_config_t *vad_cfg,
                              const struct skw_table *kw_tbl,
                              const int16_t *pcm,
                              size_t count,
                              const struct bp_env *env,
                              struct bp_replay *replay,
                              uint32_t pid,
                              uint64_t gen,
                              uint64_t nonce,
                              int16_t *tts_out,
                              size_t tts_cap,
                              struct sa_result *res)
{
    if (!res) return SA_E_ARG;
    for (size_t i = 0; i < sizeof(*res); ++i) ((uint8_t *)res)[i] = 0;
    res->status = SA_E_ARG;

    if (!vad_state || !vad_cfg || !kw_tbl || !pcm || !env || !env->caps || !replay || !tts_out)
        return SA_E_ARG;

    if (audio_vad_process_frame(vad_state, vad_cfg, pcm, count, &res->vad) != 0)
        return SA_E_ARG;

    if (!res->vad.is_speech) {
        res->status = SA_SILENCE;
        return SA_SILENCE;
    }

    if (sf_extract_frame(pcm, count, &res->feat) != SF_OK)
        return SA_E_ARG;

    uint64_t total_band = res->feat.low_band_energy + res->feat.high_band_energy;
    int8_t band_ratio_q8 = total_band > 0
        ? (int8_t)sa_clamp_q7((int32_t)((res->feat.low_band_energy * 127ULL) / total_band))
        : 0;
    struct skw_vec qv;
    qv.v[0] = res->feat.energy_q8;
    qv.v[1] = res->feat.zcr_q8;
    qv.v[2] = band_ratio_q8;
    qv.v[3] = 32; /* single-frame cue duration proxy */

    if (skw_recognize(kw_tbl, &qv, &res->kw) != SKW_OK) {
        res->status = SA_NO_MATCH;
        return SA_NO_MATCH;
    }

    const char *cue = 0;
    const char *phrase = sa_cmd_to_phrase(res->kw.cmd, &cue);
    if (!phrase || !cue) {
        res->status = SA_NO_MATCH;
        return SA_NO_MATCH;
    }

    aura_parse(phrase, sa_cstr_len(phrase), env->caps, &res->intent);
    if (!aura_validate(&res->intent, env->caps)) {
        res->status = SA_E_INTENT;
        return SA_E_INTENT;
    }

    struct bp_request req;
    req.pid = pid;
    req.gen = gen;
    req.nonce = nonce;
    req.intent = res->intent;
    bp_decide(env, replay, &req, &res->broker);

    if (res->broker.decision != BP_ALLOW_NOW) {
        res->spoken_cue = "Denied";
        if (tts_synthesize("Denied", 6, tts_out, tts_cap, &res->tts_samples) != TTS_OK) {
            res->status = SA_E_TTS;
            return SA_E_TTS;
        }
        res->status = SA_DENIED;
        return SA_DENIED;
    }

    res->spoken_cue = cue;
    if (tts_synthesize(cue, sa_cstr_len(cue), tts_out, tts_cap, &res->tts_samples) != TTS_OK) {
        res->status = SA_E_TTS;
        return SA_E_TTS;
    }

    res->status = SA_OK;
    return SA_OK;
}

#endif /* AURORA_SPEECH_ASSISTANT_H */
