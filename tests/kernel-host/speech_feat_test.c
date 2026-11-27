/* tests/kernel-host/speech_feat_test.c - host tests for kernel/include/speech_feat.h */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <speech_feat.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

int main(void)
{
    struct sf_frame_features f;
    int16_t buf[AUDIO_VAD_MAX_FRAME_SAMPLES];

    /* Bad args */
    CHECK("null samples rejected", sf_extract_frame(0, 320, &f) == SF_E_ARG);
    CHECK("null out rejected", sf_extract_frame(buf, 320, 0) == SF_E_ARG);
    CHECK("zero count rejected", sf_extract_frame(buf, 0, &f) == SF_E_BOUNDS);
    CHECK("oversized count rejected", sf_extract_frame(buf, AUDIO_VAD_MAX_FRAME_SAMPLES + 1, &f) == SF_E_BOUNDS);

    /* Silence */
    memset(buf, 0, sizeof(buf));
    CHECK("silence succeeds", sf_extract_frame(buf, 320, &f) == SF_OK);
    CHECK("silence dc is 0", f.dc_offset == 0);
    CHECK("silence energy is 0", f.mean_energy == 0 && f.low_band_energy == 0 && f.high_band_energy == 0);
    CHECK("silence peak is 0", f.peak_abs == 0);
    CHECK("silence zcr is 0", f.zcr_permille == 0 && f.zcr_q8 == 0 && f.energy_q8 == 0);

    /* Constant DC offset */
    for (size_t i = 0; i < 320; i++) buf[i] = 4321;
    CHECK("constant DC succeeds", sf_extract_frame(buf, 320, &f) == SF_OK);
    CHECK("constant DC offset is 4321", f.dc_offset == 4321);
    CHECK("constant DC residual energy is 0", f.mean_energy == 0 && f.low_band_energy == 0 && f.high_band_energy == 0);
    CHECK("constant DC zcr is 0", f.zcr_permille == 0);

    /* Low-frequency square wave (period 32 samples -> 16 high, 16 low) */
    for (size_t i = 0; i < 320; i++) buf[i] = ((i / 16) % 2 == 0) ? 4000 : -4000;
    CHECK("low-freq wave succeeds", sf_extract_frame(buf, 320, &f) == SF_OK);
    CHECK("low-freq energy > 0", f.mean_energy > 0 && f.peak_abs == 4000);
    CHECK("low-freq has low_band > high_band", f.low_band_energy > f.high_band_energy);
    CHECK("low-freq zcr < 100 permille", f.zcr_permille > 0 && f.zcr_permille < 100u);
    CHECK("low-freq quantized features in [0, 127]", f.energy_q8 > 0 && f.energy_q8 <= 127 && f.zcr_q8 >= 0 && f.zcr_q8 <= 127);

    /* High-frequency alternating wave (+4000, -4000, ...) */
    for (size_t i = 0; i < 320; i++) buf[i] = (i % 2 == 0) ? 4000 : -4000;
    CHECK("high-freq wave succeeds", sf_extract_frame(buf, 320, &f) == SF_OK);
    CHECK("high-freq has high_band > low_band", f.high_band_energy > f.low_band_energy);
    CHECK("high-freq zcr is 1000 permille", f.zcr_permille == 1000u && f.zcr_q8 == 127);

    /* Alternating signed extremes (INT16_MIN / INT16_MAX) -> no UB or overflow */
    for (size_t i = 0; i < AUDIO_VAD_MAX_FRAME_SAMPLES; i++) {
        buf[i] = (i % 2 == 0) ? INT16_MAX : INT16_MIN;
    }
    CHECK("max frame extremes succeed", sf_extract_frame(buf, AUDIO_VAD_MAX_FRAME_SAMPLES, &f) == SF_OK);
    CHECK("extremes peak_abs bounded", f.peak_abs >= 32767u);
    CHECK("extremes zcr is 1000", f.zcr_permille == 1000u);
    CHECK("extremes quantized energy saturates <= 127", f.energy_q8 > 0 && f.energy_q8 <= 127);

    /* Single sample frame */
    buf[0] = 1000;
    CHECK("single sample frame succeeds", sf_extract_frame(buf, 1, &f) == SF_OK && f.zcr_permille == 0);

    printf("speech_feat: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
