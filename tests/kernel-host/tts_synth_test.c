/* tests/kernel-host/tts_synth_test.c - host tests for kernel/include/tts_synth.h */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <tts_synth.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

int main(void)
{
    int16_t pcm[TTS_MAX_SAMPLES];
    size_t n = 999;

    /* Bad args */
    CHECK("null text rejected", tts_synthesize(0, 4, pcm, TTS_MAX_SAMPLES, &n) == TTS_E_ARG && n == 0);
    CHECK("null out rejected", tts_synthesize("Home", 4, 0, TTS_MAX_SAMPLES, &n) == TTS_E_ARG);
    CHECK("null out_len rejected", tts_synthesize("Home", 4, pcm, TTS_MAX_SAMPLES, 0) == TTS_E_ARG);
    CHECK("zero len rejected", tts_synthesize("Home", 0, pcm, TTS_MAX_SAMPLES, &n) == TTS_E_BOUNDS);
    CHECK("oversized len rejected", tts_synthesize("Home", TTS_MAX_PHRASE + 1, pcm, TTS_MAX_SAMPLES, &n) == TTS_E_BOUNDS);
    CHECK("small cap rejected", tts_synthesize("Home", 4, pcm, 100, &n) == TTS_E_BOUNDS);

    /* Non-ASCII / control char rejected */
    const char bad_ctrl[3] = { 'H', '\n', 'i' };
    const char bad_utf8[3] = { 'H', (char)0xC3, 'i' };
    CHECK("control char rejected", tts_synthesize(bad_ctrl, 3, pcm, TTS_MAX_SAMPLES, &n) == TTS_E_ENCODING);
    CHECK("non-ASCII rejected", tts_synthesize(bad_utf8, 3, pcm, TTS_MAX_SAMPLES, &n) == TTS_E_ENCODING);

    /* Spaces produce silence */
    CHECK("spaces synthesize", tts_synthesize("   ", 3, pcm, TTS_MAX_SAMPLES, &n) == TTS_OK && n == 3 * TTS_SAMPLES_PER_CHAR);
    int all_zero = 1;
    for (size_t i = 0; i < n; i++) if (pcm[i] != 0) all_zero = 0;
    CHECK("spaces are pure silence", all_zero == 1);

    /* Phrase synthesis: "Timer finished" */
    const char *phrase = "Timer finished";
    CHECK("phrase synthesizes", tts_synthesize(phrase, strlen(phrase), pcm, TTS_MAX_SAMPLES, &n) == TTS_OK);
    CHECK("sample count matches 14 * 160", n == 14u * TTS_SAMPLES_PER_CHAR);
    int16_t min_s = 32767, max_s = -32768;
    for (size_t i = 0; i < n; i++) {
        if (pcm[i] < min_s) min_s = pcm[i];
        if (pcm[i] > max_s) max_s = pcm[i];
    }
    CHECK("phrase has non-zero audio", max_s > 1000 && min_s < -1000);
    CHECK("phrase bounded within TTS_AMP_MAX", max_s <= TTS_AMP_MAX && min_s >= -TTS_AMP_MAX);
    /* Envelope starts at 0 and ends at 0 */
    CHECK("first sample is 0 (attack)", pcm[0] == 0);
    CHECK("last sample is 0 (decay)", pcm[n - 1] == 0);

    /* Determinism */
    int16_t pcm2[TTS_MAX_SAMPLES];
    size_t n2 = 0;
    CHECK("second run succeeds", tts_synthesize(phrase, strlen(phrase), pcm2, TTS_MAX_SAMPLES, &n2) == TTS_OK && n2 == n);
    CHECK("deterministic output", memcmp(pcm, pcm2, n * sizeof(int16_t)) == 0);

    printf("tts_synth: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
