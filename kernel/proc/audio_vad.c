#include "audio_vad.h"

static int16_t clamp_i16(int64_t v) {
    if (v > INT16_MAX) {
        return INT16_MAX;
    }
    if (v < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)v;
}

static uint32_t abs_i64_to_u32(int64_t v) {
    if (v < 0) {
        v = -v;
    }
    if (v > (int64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)v;
}

void audio_vad_default_config(audio_vad_config_t *cfg) {
    if (!cfg) {
        return;
    }
    cfg->energy_threshold = AUDIO_VAD_DEFAULT_THRESHOLD;
    cfg->hangover_frames = AUDIO_VAD_DEFAULT_HANGOVER;
}

int audio_vad_init(audio_vad_state_t *state, const audio_vad_config_t *cfg) {
    if (!state || !cfg) {
        return -1;
    }
    audio_vad_reset(state);
    return 0;
}

void audio_vad_reset(audio_vad_state_t *state) {
    if (!state) {
        return;
    }
    state->frame_count = 0;
    state->speech_frames = 0;
    state->silence_frames = 0;
    state->last_energy = 0;
    state->last_dc_offset = 0;
    state->hangover_remaining = 0;
    state->active = false;
}

int audio_vad_remove_dc(const int16_t *in_samples,
                        int16_t *out_samples,
                        size_t count,
                        int32_t *out_dc_offset) {
    if (!in_samples || count == 0 || count > AUDIO_VAD_MAX_FRAME_SAMPLES) {
        return -1;
    }

    int64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += (int64_t)in_samples[i];
    }

    int32_t dc = (int32_t)(sum / (int64_t)count);
    if (out_dc_offset) {
        *out_dc_offset = dc;
    }

    if (out_samples) {
        for (size_t i = 0; i < count; i++) {
            int64_t centered = (int64_t)in_samples[i] - (int64_t)dc;
            out_samples[i] = clamp_i16(centered);
        }
    }

    return 0;
}

int audio_vad_compute_energy(const int16_t *samples,
                             size_t count,
                             int32_t dc_offset,
                             uint64_t *out_mean_energy,
                             uint32_t *out_peak_abs) {
    if (!samples || count == 0 || count > AUDIO_VAD_MAX_FRAME_SAMPLES || !out_mean_energy) {
        return -1;
    }

    uint64_t energy_sum = 0;
    uint32_t peak = 0;

    for (size_t i = 0; i < count; i++) {
        int64_t diff = (int64_t)samples[i] - (int64_t)dc_offset;
        uint32_t mag = abs_i64_to_u32(diff);
        if (mag > peak) {
            peak = mag;
        }
        energy_sum += (uint64_t)mag * (uint64_t)mag;
    }

    *out_mean_energy = energy_sum / (uint64_t)count;
    if (out_peak_abs) {
        *out_peak_abs = peak;
    }

    return 0;
}

int audio_vad_process_frame(audio_vad_state_t *state,
                            const audio_vad_config_t *cfg,
                            const int16_t *samples,
                            size_t count,
                            audio_vad_result_t *out_result) {
    if (!state || !cfg || !samples || count == 0 || count > AUDIO_VAD_MAX_FRAME_SAMPLES) {
        return -1;
    }

    int32_t dc = 0;
    if (audio_vad_remove_dc(samples, NULL, count, &dc) != 0) {
        return -1;
    }

    uint64_t mean_energy = 0;
    uint32_t peak_abs = 0;
    if (audio_vad_compute_energy(samples, count, dc, &mean_energy, &peak_abs) != 0) {
        return -1;
    }

    bool above_thresh = (mean_energy >= cfg->energy_threshold);
    bool is_speech = false;

    if (above_thresh) {
        is_speech = true;
        state->hangover_remaining = cfg->hangover_frames;
    } else if (state->hangover_remaining > 0) {
        is_speech = true;
        state->hangover_remaining--;
    }

    state->frame_count++;
    if (is_speech) {
        state->speech_frames++;
    } else {
        state->silence_frames++;
    }
    state->last_energy = mean_energy;
    state->last_dc_offset = dc;
    state->active = is_speech;

    if (out_result) {
        out_result->dc_offset = dc;
        out_result->mean_energy = mean_energy;
        out_result->peak_abs = peak_abs;
        out_result->is_speech = is_speech;
    }

    return 0;
}
