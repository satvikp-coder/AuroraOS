#include <model_signed_volume.h>
#include <sha256.h>
#include <stdio.h>
#include <string.h>
#include "monocypher-ed25519.h"

struct image { uint8_t sectors[4][MS_SECTOR]; };

static int read_sectors(void *ctx, uint64_t first, uint32_t count, uint8_t *out)
{
    struct image *image = ctx;
    if (first >= 4 || count > 4 - first) return -1;
    memcpy(out, image->sectors[first], count * MS_SECTOR);
    return 0;
}

static void put32(uint8_t *p, uint32_t x)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(x >> (8 * i));
}

static void digest(const uint8_t *bytes, size_t len, uint8_t *out)
{
    struct sha256 hash;
    sha256_init(&hash);
    sha256_update(&hash, bytes, len);
    sha256_final(&hash, out);
}

static void seal(struct image *image, const uint8_t key[64])
{
    uint8_t *header = image->sectors[0], *trailer = image->sectors[3];
    digest(image->sectors[1], MS_ENTRY, header + 64);
    digest(header, 96, header + 96);
    memcpy(trailer + 16, header + 96, 32);
    crypto_ed25519_sign(trailer + 48, key, trailer, MT_SIGNED_BYTES);
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed at line %d\n", __LINE__); return 1; } } while (0)

int main(void)
{
    struct image image = {0};
    struct mt_trust_table trust = {.count = 1, .min_model_version = 7};
    uint8_t seed[32], key[64];
    FILE *entropy = fopen("/dev/urandom", "rb");
    CHECK(entropy && fread(seed, 1, sizeof(seed), entropy) == sizeof(seed));
    CHECK(fclose(entropy) == 0);
    trust.keys[0].key_id = 3;
    trust.keys[0].active = 1;
    crypto_ed25519_key_pair(key, trust.keys[0].pubkey, seed);
    uint8_t *h = image.sectors[0], *t = image.sectors[1], *sig = image.sectors[3];
    memcpy(h, "AURMODEL", 8);
    put32(h + 8, 2); put32(h + 12, MS_SECTOR); put32(h + 16, 4);
    put32(h + 24, 1); put32(h + 28, 1); put32(h + 32, 1);
    memcpy(t, "weights", 7);
    put32(t + 32, 1); put32(t + 40, 2); put32(t + 48, 1);
    image.sectors[2][0] = 42;
    digest(image.sectors[2], 1, t + 64);
    memcpy(sig, "AURS", 4);
    put32(sig + 4, 2); put32(sig + 8, 3); put32(sig + 12, 7);
    seal(&image, key);

    struct ms_signed_scratch scratch;
    struct ms_volume volume;
#define VERIFY() ms_verify_signed_volume(read_sectors, &image, 4, &trust, &scratch, &volume)
#define PREFLIGHT() ms_preflight_signed(read_sectors, &image, 4, &trust, &scratch, &volume)
    CHECK(PREFLIGHT() == MS_OK && volume.nobjects == 1);
    CHECK(VERIFY() == MS_OK && volume.nobjects == 1);
    CHECK(ms_verify_object(&volume, 0, read_sectors, &image) == MS_OK);
    image.sectors[2][0] ^= 1;
    CHECK(PREFLIGHT() == MS_OK);
    CHECK(ms_verify_object(&volume, 0, read_sectors, &image) == MS_EDATA);
    volume.nobjects = 99;
    CHECK(VERIFY() == MS_EDATA && volume.nobjects == 99);
    image.sectors[2][0] ^= 1;
    volume.nobjects = 99;
    image.sectors[2][MS_SECTOR - 1] = 1;
    CHECK(VERIFY() == MS_EDATA && volume.nobjects == 99);
    image.sectors[2][MS_SECTOR - 1] = 0;
    sig[50] ^= 1;
    CHECK(PREFLIGHT() == MS_EAUTH && volume.nobjects == 99);
    CHECK(VERIFY() == MS_EAUTH && volume.nobjects == 99);
    sig[50] ^= 1;
    sig[MT_TRAILER_BYTES] = 1;
    CHECK(PREFLIGHT() == MS_EFORMAT);
    sig[MT_TRAILER_BYTES] = 0;
    sig[0] ^= 1;
    CHECK(PREFLIGHT() == MS_EAUTH);
    sig[0] ^= 1;
    trust.keys[0].pubkey[0] ^= 1;
    CHECK(PREFLIGHT() == MS_EAUTH);
    trust.keys[0].pubkey[0] ^= 1;
    trust.count = 0;
    CHECK(PREFLIGHT() == MS_EAUTH);
    trust.count = 1;
    trust.keys[0].active = 0;
    CHECK(PREFLIGHT() == MS_EAUTH);
    trust.keys[0].active = 1;
    trust.min_model_version = 8;
    CHECK(PREFLIGHT() == MS_EAUTH);
    trust.min_model_version = 7;
    put32(sig + 12, 8);
    CHECK(PREFLIGHT() == MS_EAUTH);
    put32(sig + 12, 7);
    CHECK(ms_preflight_signed(read_sectors, &image, 3, &trust, &scratch, &volume) == MS_EGEOM);
    put32(h + 8, 1);
    CHECK(PREFLIGHT() == MS_EVERSION);
    put32(h + 8, 2);
    CHECK(ms_parse(h, t, MS_SECTOR, &volume) == MS_EVERSION);
    put32(t + 40, 3);
    seal(&image, key);
    CHECK(PREFLIGHT() == MS_EEXTENT);
    put32(t + 40, 2);
    seal(&image, key);
    crypto_wipe(key, sizeof(key));
    t[0] = 'W';
    digest(t, MS_ENTRY, h + 64);
    digest(h, 96, h + 96);
    memcpy(sig + 16, h + 96, 32);
    CHECK(PREFLIGHT() == MS_EAUTH);
    puts("signed model: publisher preflight, geometry and separate payload verification passed");
    return 0;
}
