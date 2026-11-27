/*
 * kernel/include/speech_pipeline.h - End-to-end speech-to-Aura pipeline (SPEECH + AURA).
 *
 * Connects:
 *   PCM16 frame -> audio_vad_process_frame() -> fixed-point ZCR/energy feature classification
 *   -> canonical command text -> aura_parse() -> aura_validate() -> bp_decide()
 *
 * Fail-closed:
 *   - Silence / non-speech frames stop at VAD (no Aura intent proposed).
 *   - Unknown/ambiguous acoustic patterns stop before Aura.
 *   - Every proposed command passes through aura_parse() + aura_validate() + bp_decide()
 *     with caller identity, generation, and replay nonce checked by the Action Broker.
 *   - Voice/speech NEVER bypasses S1 review or S3 hold-to-confirm, and unprivileged EL0
 *     callers cannot invoke shell-only commands (AK_OPEN_APP, AK_TIMER).
 */
#ifndef AURORA_SPEECH_PIPELINE_H
#define AURORA_SPEECH_PIPELINE_H

#include <stddef.h>
#include <stdint.h>
#include <audio_vad.h>
#include <aura_cmd.h>
#include <broker_policy.h>

enum sp_stage_status {
    SP_OK = 0,
    SP_E_ARG = -1,
    SP_E_SILENCE = -2,
    SP_E_NO_MATCH = -3,
    SP_E_INTENT = -4,
    SP_E_BROKER = -5
};

struct sp_pipeline_out {
    int32_t stage_status;
    audio_vad_result_t vad;
    uint32_t zcr_permille;
    const char *matched_phrase;
    struct aura_intent intent;
    struct bp_result broker;
};

static inline uint32_t sp_compute_zcr(const int16_t *samples, size_t count, int32_t dc)
{
    if (!samples || count < 2) return 0u;
    uint32_t cross = 0;
    int32_t prev = (int32_t)samples[0] - dc;
    for (size_t i = 1; i < count; i++) {
        int32_t cur = (int32_t)samples[i] - dc;
        if ((prev >= 0 && cur < 0) || (prev < 0 && cur >= 0)) cross++;
        prev = cur;
    }
    return (uint32_t)(((uint64_t)cross * 1000ULL) / (uint64_t)(count - 1));
}

/* Classify active speech frame by fixed-point ZCR band into a canonical Aura command string:
 *   low  ZCR [20..150]  -> "open home"
 *   mid  ZCR [200..450] -> "open notes"
 *   high ZCR [600..900] -> "cancel timer" */
static inline const char *sp_classify_zcr(uint32_t zcr_permille)
{
    if (zcr_permille >= 20u && zcr_permille <= 150u) return "open home";
    if (zcr_permille >= 200u && zcr_permille <= 450u) return "open notes";
    if (zcr_permille >= 600u && zcr_permille <= 900u) return "cancel timer";
    return 0;
}

static inline size_t sp_cstr_len(const char *s)
{
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static inline int sp_process_pcm_to_broker(audio_vad_state_t *vad_state,
                                           const audio_vad_config_t *vad_cfg,
                                           const int16_t *pcm,
                                           size_t count,
                                           const struct bp_env *env,
                                           struct bp_replay *replay,
                                           uint32_t pid,
                                           uint64_t gen,
                                           uint64_t nonce,
                                           struct sp_pipeline_out *out)
{
    if (!out) return SP_E_ARG;
    for (size_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;
    out->stage_status = SP_E_ARG;

    if (!vad_state || !vad_cfg || !pcm || !env || !env->caps || !replay) return SP_E_ARG;
    if (audio_vad_process_frame(vad_state, vad_cfg, pcm, count, &out->vad) != 0) return SP_E_ARG;

    if (!out->vad.is_speech) {
        out->stage_status = SP_E_SILENCE;
        return SP_E_SILENCE;
    }

    out->zcr_permille = sp_compute_zcr(pcm, count, out->vad.dc_offset);
    out->matched_phrase = sp_classify_zcr(out->zcr_permille);
    if (!out->matched_phrase) {
        out->stage_status = SP_E_NO_MATCH;
        return SP_E_NO_MATCH;
    }

    aura_parse(out->matched_phrase, sp_cstr_len(out->matched_phrase), env->caps, &out->intent);
    if (!aura_validate(&out->intent, env->caps)) {
        out->stage_status = SP_E_INTENT;
        return SP_E_INTENT;
    }

    struct bp_request req;
    req.pid = pid;
    req.gen = gen;
    req.nonce = nonce;
    req.intent = out->intent;
    bp_decide(env, replay, &req, &out->broker);

    if (out->broker.decision != BP_ALLOW_NOW) {
        out->stage_status = SP_E_BROKER;
        return SP_E_BROKER;
    }

    out->stage_status = SP_OK;
    return SP_OK;
}

#endif /* AURORA_SPEECH_PIPELINE_H */
