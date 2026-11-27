#ifndef AURORA_AUDIO_VAD_H
#define AURORA_AUDIO_VAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 20 ms @ 16 kHz mono = 320 samples; cap bounded frame processing at 1024. */
#define AUDIO_VAD_DEFAULT_FRAME_SAMPLES 320U
#define AUDIO_VAD_MAX_FRAME_SAMPLES     1024U
#define AUDIO_VAD_DEFAULT_THRESHOLD     50000ULL
#define AUDIO_VAD_DEFAULT_HANGOVER      3U

typedef struct {
    uint64_t energy_threshold;
    uint32_t hangover_frames;
} audio_vad_config_t;

typedef struct {
    uint64_t frame_count;
    uint64_t speech_frames;
    uint64_t silence_frames;
    uint64_t last_energy;
    int32_t  last_dc_offset;
    uint32_t hangover_remaining;
    bool     active;
} audio_vad_state_t;

typedef struct {
    int32_t  dc_offset;
    uint64_t mean_energy;
    uint32_t peak_abs;
    bool     is_speech;
} audio_vad_result_t;

void audio_vad_default_config(audio_vad_config_t *cfg);
int audio_vad_init(audio_vad_state_t *state, const audio_vad_config_t *cfg);
void audio_vad_reset(audio_vad_state_t *state);

int audio_vad_remove_dc(const int16_t *in_samples,
                        int16_t *out_samples,
                        size_t count,
                        int32_t *out_dc_offset);

int audio_vad_compute_energy(const int16_t *samples,
                             size_t count,
                             int32_t dc_offset,
                             uint64_t *out_mean_energy,
                             uint32_t *out_peak_abs);

int audio_vad_process_frame(audio_vad_state_t *state,
                            const audio_vad_config_t *cfg,
                            const int16_t *samples,
                            size_t count,
                            audio_vad_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif /* AURORA_AUDIO_VAD_H */
