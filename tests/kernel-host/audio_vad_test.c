#include "audio_vad.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    tests_run++; \
    fn(); \
    tests_passed++; \
} while (0)

static void test_null_and_bounds(void) {
    audio_vad_config_t cfg;
    audio_vad_state_t state;
    audio_vad_result_t res;
    int16_t buf[8] = {0};
    int16_t out[8] = {0};
    int32_t dc = 0;
    uint64_t energy = 0;
    uint32_t peak = 0;

    audio_vad_default_config(&cfg);
    assert(audio_vad_init(NULL, &cfg) == -1);
    assert(audio_vad_init(&state, NULL) == -1);
    assert(audio_vad_init(&state, &cfg) == 0);

    assert(audio_vad_remove_dc(NULL, out, 8, &dc) == -1);
    assert(audio_vad_remove_dc(buf, out, 0, &dc) == -1);
    assert(audio_vad_remove_dc(buf, out, AUDIO_VAD_MAX_FRAME_SAMPLES + 1U, &dc) == -1);

    assert(audio_vad_compute_energy(NULL, 8, 0, &energy, &peak) == -1);
    assert(audio_vad_compute_energy(buf, 0, 0, &energy, &peak) == -1);
    assert(audio_vad_compute_energy(buf, AUDIO_VAD_MAX_FRAME_SAMPLES + 1U, 0, &energy, &peak) == -1);
    assert(audio_vad_compute_energy(buf, 8, 0, NULL, &peak) == -1);

    assert(audio_vad_process_frame(NULL, &cfg, buf, 8, &res) == -1);
    assert(audio_vad_process_frame(&state, NULL, buf, 8, &res) == -1);
    assert(audio_vad_process_frame(&state, &cfg, NULL, 8, &res) == -1);
    assert(audio_vad_process_frame(&state, &cfg, buf, 0, &res) == -1);
    assert(audio_vad_process_frame(&state, &cfg, buf, AUDIO_VAD_MAX_FRAME_SAMPLES + 1U, &res) == -1);
}

static void test_dc_offset_removal_and_clamp(void) {
    int16_t in[4] = {1000, 1010, 990, 1000};
    int16_t out[4] = {0};
    int32_t dc = 0;

    assert(audio_vad_remove_dc(in, out, 4, &dc) == 0);
    assert(dc == 1000);
    assert(out[0] == 0);
    assert(out[1] == 10);
    assert(out[2] == -10);
    assert(out[3] == 0);

    /* Extreme values: ensure no signed overflow and proper clamping. */
    int16_t extreme[2] = {INT16_MIN, INT16_MAX};
    int16_t extreme_out[2] = {0};
    assert(audio_vad_remove_dc(extreme, extreme_out, 2, &dc) == 0);
    assert(dc == 0);
    assert(extreme_out[0] == INT16_MIN);
    assert(extreme_out[1] == INT16_MAX);
}

static void test_overflow_safe_energy(void) {
    int16_t max_frame[AUDIO_VAD_MAX_FRAME_SAMPLES];
    for (size_t i = 0; i < AUDIO_VAD_MAX_FRAME_SAMPLES; i++) {
        max_frame[i] = (i & 1U) ? INT16_MAX : INT16_MIN;
    }

    int32_t dc = 0;
    uint64_t energy = 0;
    uint32_t peak = 0;
    assert(audio_vad_remove_dc(max_frame, NULL, AUDIO_VAD_MAX_FRAME_SAMPLES, &dc) == 0);
    assert(audio_vad_compute_energy(max_frame, AUDIO_VAD_MAX_FRAME_SAMPLES, dc, &energy, &peak) == 0);
    assert(energy > 1000000000ULL);
    assert(peak >= 32767U);
}

static void test_silence_vs_speech_classification(void) {
    audio_vad_config_t cfg;
    audio_vad_state_t state;
    audio_vad_result_t res;

    audio_vad_default_config(&cfg);
    cfg.energy_threshold = 50000ULL;
    cfg.hangover_frames = 2U;
    assert(audio_vad_init(&state, &cfg) == 0);

    /* Silent frame with constant DC offset should have 0 AC energy -> silence. */
    int16_t silent_dc[AUDIO_VAD_DEFAULT_FRAME_SAMPLES];
    for (size_t i = 0; i < AUDIO_VAD_DEFAULT_FRAME_SAMPLES; i++) {
        silent_dc[i] = 1234;
    }
    assert(audio_vad_process_frame(&state, &cfg, silent_dc, AUDIO_VAD_DEFAULT_FRAME_SAMPLES, &res) == 0);
    assert(res.dc_offset == 1234);
    assert(res.mean_energy == 0);
    assert(res.peak_abs == 0);
    assert(res.is_speech == false);
    assert(state.silence_frames == 1);

    /* Speech-like synthetic frame: alternating +/- 1000 around DC 500. */
    int16_t speech[AUDIO_VAD_DEFAULT_FRAME_SAMPLES];
    for (size_t i = 0; i < AUDIO_VAD_DEFAULT_FRAME_SAMPLES; i++) {
        speech[i] = (int16_t)(500 + ((i & 1U) ? 1000 : -1000));
    }
    assert(audio_vad_process_frame(&state, &cfg, speech, AUDIO_VAD_DEFAULT_FRAME_SAMPLES, &res) == 0);
    assert(res.dc_offset == 500);
    assert(res.mean_energy == 1000000ULL);
    assert(res.peak_abs == 1000U);
    assert(res.is_speech == true);
    assert(state.speech_frames == 1);

    /* Hangover: next 2 silent frames stay active, 3rd drops to silence. */
    assert(audio_vad_process_frame(&state, &cfg, silent_dc, AUDIO_VAD_DEFAULT_FRAME_SAMPLES, &res) == 0);
    assert(res.is_speech == true);
    assert(audio_vad_process_frame(&state, &cfg, silent_dc, AUDIO_VAD_DEFAULT_FRAME_SAMPLES, &res) == 0);
    assert(res.is_speech == true);
    assert(audio_vad_process_frame(&state, &cfg, silent_dc, AUDIO_VAD_DEFAULT_FRAME_SAMPLES, &res) == 0);
    assert(res.is_speech == false);
    assert(state.frame_count == 5);
    assert(state.speech_frames == 3);
    assert(state.silence_frames == 2);
}

int main(void) {
    RUN_TEST(test_null_and_bounds);
    RUN_TEST(test_dc_offset_removal_and_clamp);
    RUN_TEST(test_overflow_safe_energy);
    RUN_TEST(test_silence_vs_speech_classification);

    printf("audio_vad_test: %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
