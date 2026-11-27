/* Pure Ed25519 (SHA-512) detached verification against all RFC 8032 7.1 vectors. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "monocypher-ed25519.h"

struct vector { const char *name, *key, *message, *signature; };
static const struct vector vectors[] = {
#include "ed25519_vectors.inc"
};

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int decode(const char *hex, uint8_t *bytes, size_t capacity)
{
    size_t length = strlen(hex);
    if (length % 2 || length / 2 > capacity) return -1;
    for (size_t i = 0; i < length / 2; ++i) {
        int hi = nibble(hex[i * 2]), lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(length / 2);
}

static int reject_bit_flips(uint8_t *field, size_t length,
                            const uint8_t *sig, const uint8_t *pk,
                            const uint8_t *msg, size_t msg_len)
{
    for (size_t i = 0; i < length; ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            field[i] ^= (uint8_t)(1u << bit);
            int result = crypto_ed25519_check(sig, pk, msg, msg_len);
            field[i] ^= (uint8_t)(1u << bit);
            if (result == 0) return 0;
        }
    }
    return 1;
}

int main(void)
{
    size_t flips = 0;
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        const struct vector *v = &vectors[i];
        uint8_t pk[32], sig[64], msg[1023];
        int len = decode(v->message, msg, sizeof(msg));
        if (decode(v->key, pk, sizeof(pk)) != 32 ||
            decode(v->signature, sig, sizeof(sig)) != 64 || len < 0 ||
            crypto_ed25519_check(sig, pk, msg, (size_t)len) != 0 ||
            !reject_bit_flips(pk, sizeof(pk), sig, pk, msg, len) ||
            !reject_bit_flips(sig, sizeof(sig), sig, pk, msg, len) ||
            !reject_bit_flips(msg, (size_t)len, sig, pk, msg, len)) {
            fprintf(stderr, "Ed25519 RFC 8032 vector %s failed\n", v->name);
            return 1;
        }
        flips += (sizeof(sig) + sizeof(pk) + (size_t)len) * 8;
        /* S >= group order cannot be a valid canonical signature scalar. */
        memset(sig + 32, 0xff, 32);
        if (crypto_ed25519_check(sig, pk, msg, (size_t)len) == 0) return 1;
        memset(sig, 0, sizeof(sig));
        if (crypto_ed25519_check(sig, pk, msg, (size_t)len) == 0) return 1;
    }
    printf("Ed25519: 5 RFC vectors passed, %zu single-bit mutations rejected\n", flips);
    return 0;
}
