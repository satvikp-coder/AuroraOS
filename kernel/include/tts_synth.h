/*
 * kernel/include/tts_synth.h - Lightweight native offline fixed-point TTS cue synthesizer.
 *
 * Pure C, freestanding, no floating point, no libc, no dynamic allocation, no globals.
 * Synthesizes bounded 16 kHz mono PCM16 audio frames from short ASCII phrases using
 * deterministic phoneme-class pitch/formant tone envelopes (no clicks, bounded amplitude).
 */
#ifndef AURORA_TTS_SYNTH_H
#define AURORA_TTS_SYNTH_H

#include <stddef.h>
#include <stdint.h>

#define TTS_SAMPLE_RATE      16000u
#define TTS_SAMPLES_PER_CHAR 160u    /* 10 ms per character at 16 kHz */
#define TTS_MAX_PHRASE       64u
#define TTS_MAX_SAMPLES      4096u
#define TTS_AMP_MAX          8000    /* bounded PCM16 amplitude well below INT16_MAX */

enum tts_err {
    TTS_OK = 0,
    TTS_E_ARG = -1,
    TTS_E_BOUNDS = -2,
    TTS_E_ENCODING = -3
};

/* Q16.16 fixed-point Bhaskara sine, matching virtio_snd.c. Input angle in [0, 2*PI_Q16). */
static inline int32_t tts_sin_q16(int32_t x)
{
    const int32_t PI = 0x3243F;
    const int32_t TWO_PI = 0x6487E;
    const int32_t PI_HALF = 0x1921F;

    x %= TWO_PI;
    if (x < 0) x += TWO_PI;

    int neg = 0;
    if (x > PI) { x -= PI; neg = 1; }
    if (x > PI_HALF) x = PI - x;

    int64_t xl = (int64_t)x;
    int64_t pl = (int64_t)PI;
    int64_t pmx = pl - xl;
    int64_t num = 16 * xl * pmx;
    int64_t den = 5 * pl * pl - 4 * xl * pmx;
    int64_t res = den ? ((num << 16) / den) : 0;
    return neg ? -(int32_t)res : (int32_t)res;
}

/* Map ASCII character to base frequency in Hz (0 = silence for spaces/punctuation). */
static inline uint32_t tts_char_freq(char c)
{
    if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    if (c == ' ' || c == '\t' || c == '.' || c == ',' || c == '!' || c == '?') return 0u;
    if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') {
        /* Vowels: resonant lower formants 180..300 Hz */
        return 180u + (uint32_t)((unsigned char)(c - 'a') * 5u);
    }
    if (c >= '0' && c <= '9') {
        return 320u + (uint32_t)(c - '0') * 20u;
    }
    if (c >= 'a' && c <= 'z') {
        return 260u + (uint32_t)(c - 'a') * 12u;
    }
    return 0u;
}

/* Synthesize PCM16 mono samples for `text` (len bytes) into `out` (cap samples).
 * Writes sample count to `*out_len`. Rejects non-printable ASCII. */
static inline int tts_synthesize(const char *text,
                                 size_t len,
                                 int16_t *out,
                                 size_t cap,
                                 size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!text || !out || !out_len) return TTS_E_ARG;
    if (len == 0 || len > TTS_MAX_PHRASE) return TTS_E_BOUNDS;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20u || ch > 0x7Eu) return TTS_E_ENCODING;
    }

    size_t need = len * TTS_SAMPLES_PER_CHAR;
    if (need > TTS_MAX_SAMPLES || need > cap) return TTS_E_BOUNDS;

    const int32_t TWO_PI = 0x6487E;
    int32_t phase = 0;

    for (size_t i = 0; i < len; i++) {
        uint32_t freq = tts_char_freq(text[i]);
        int32_t step = (int32_t)(((int64_t)freq * (int64_t)TWO_PI) / (int64_t)TTS_SAMPLE_RATE);
        for (size_t s = 0; s < TTS_SAMPLES_PER_CHAR; s++) {
            int16_t sample = 0;
            if (freq > 0) {
                /* Linear 16-sample attack and 16-sample decay envelope per phoneme slot */
                int32_t env = TTS_AMP_MAX;
                if (s < 16u) env = (TTS_AMP_MAX * (int32_t)s) / 16;
                else if (s >= TTS_SAMPLES_PER_CHAR - 16u)
                    env = (TTS_AMP_MAX * (int32_t)(TTS_SAMPLES_PER_CHAR - 1u - s)) / 16;

                int32_t sin_val = tts_sin_q16(phase);
                int32_t val = (int32_t)(((int64_t)sin_val * (int64_t)env) >> 16);
                if (val > TTS_AMP_MAX) val = TTS_AMP_MAX;
                if (val < -TTS_AMP_MAX) val = -TTS_AMP_MAX;
                sample = (int16_t)val;
                phase = (phase + step) % TWO_PI;
            } else {
                phase = 0;
            }
            out[i * TTS_SAMPLES_PER_CHAR + s] = sample;
        }
    }

    *out_len = need;
    return TTS_OK;
}

#endif /* AURORA_TTS_SYNTH_H */
