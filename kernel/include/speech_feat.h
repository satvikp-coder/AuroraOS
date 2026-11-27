/*
 * kernel/include/speech_feat.h - Bounded fixed-point speech feature extraction (SPEECH-02).
 *
 * Pure, freestanding, no allocation, no floating point, no libc, no globals.
 * Operates on PCM16 mono frames (up to AUDIO_VAD_MAX_FRAME_SAMPLES = 1024) using
 * audio_vad.h DC-offset removal and energy computation, plus fixed-point ZCR and
 * 2-band (low-pass / high-pass) energy proxies suitable for int8 keyword spotting.
 */
#ifndef AURORA_SPEECH_FEAT_H
#define AURORA_SPEECH_FEAT_H

#include <stddef.h>
#include <stdint.h>
#include <audio_vad.h>

enum sf_err {
    SF_OK = 0,
    SF_E_ARG = -1,
    SF_E_BOUNDS = -2
};

struct sf_frame_features {
    int32_t  dc_offset;
    uint64_t mean_energy;
    uint64_t low_band_energy;   /* mean squared of (x[i] + x[i-1]) / 2 */
    uint64_t high_band_energy;  /* mean squared of (x[i] - x[i-1]) / 2 */
    uint32_t peak_abs;
    uint32_t zcr_permille;      /* 0..1000 zero-crossing rate */
    int8_t   energy_q8;         /* 0..127 quantized log2-energy proxy */
    int8_t   zcr_q8;            /* 0..127 quantized ZCR */
};

static inline int8_t sf_quantize_energy(uint64_t mean_energy)
{
    if (mean_energy == 0) return 0;
    uint32_t bits = 0;
    uint64_t v = mean_energy;
    while (v > 1 && bits < 63u) {
        v >>= 1;
        bits++;
    }
    /* Map bit index 0..30 (up to ~10^9 for PCM16^2) into 1..127 */
    uint32_t q = 1u + (bits * 4u);
    if (q > 127u) q = 127u;
    return (int8_t)q;
}

static inline int sf_extract_frame(const int16_t *samples,
                                   size_t count,
                                   struct sf_frame_features *out)
{
    if (!samples || !out) return SF_E_ARG;
    if (count == 0 || count > AUDIO_VAD_MAX_FRAME_SAMPLES) return SF_E_BOUNDS;

    int16_t clean[AUDIO_VAD_MAX_FRAME_SAMPLES];
    int32_t dc = 0;
    if (audio_vad_remove_dc(samples, clean, count, &dc) != 0) return SF_E_ARG;

    uint64_t mean_e = 0;
    uint32_t peak = 0;
    if (audio_vad_compute_energy(samples, count, dc, &mean_e, &peak) != 0) return SF_E_ARG;

    uint32_t crossings = 0;
    uint64_t low_sum = 0;
    uint64_t high_sum = 0;

    for (size_t i = 1; i < count; i++) {
        int32_t prev = (int32_t)clean[i - 1];
        int32_t cur = (int32_t)clean[i];
        if ((prev >= 0 && cur < 0) || (prev < 0 && cur >= 0)) {
            crossings++;
        }
        int32_t lp = (cur + prev) / 2;
        int32_t hp = (cur - prev) / 2;
        low_sum += (uint64_t)((int64_t)lp * (int64_t)lp);
        high_sum += (uint64_t)((int64_t)hp * (int64_t)hp);
    }

    uint32_t pairs = count > 1 ? (uint32_t)(count - 1) : 1u;
    out->dc_offset = dc;
    out->mean_energy = mean_e;
    out->low_band_energy = count > 1 ? (low_sum / (uint64_t)pairs) : mean_e;
    out->high_band_energy = count > 1 ? (high_sum / (uint64_t)pairs) : 0ULL;
    out->peak_abs = peak;
    out->zcr_permille = count > 1 ? (uint32_t)(((uint64_t)crossings * 1000ULL) / (uint64_t)pairs) : 0u;
    if (out->zcr_permille > 1000u) out->zcr_permille = 1000u;
    out->energy_q8 = sf_quantize_energy(mean_e);
    uint32_t zq = (out->zcr_permille * 127u) / 1000u;
    if (zq > 127u) zq = 127u;
    out->zcr_q8 = (int8_t)zq;

    return SF_OK;
}

#endif /* AURORA_SPEECH_FEAT_H */
