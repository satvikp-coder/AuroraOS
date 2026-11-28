/* Standalone test executable. Its ephemeral public root is generated at build time. */
#include <model_signed_volume.h>
#include "signed_fixture.h"

struct fixture { uint32_t flip; };

static int read_fixture(void *ctx, uint64_t sector, uint32_t count, uint8_t *out)
{
    const struct fixture *fixture = ctx;
    if (sector >= 4 || count > 4 - sector) return -1;
    for (uint32_t i = 0; i < count * MS_SECTOR; ++i) {
        uint32_t offset = (uint32_t)sector * MS_SECTOR + i;
        out[i] = signed_image[offset] ^ (offset == fixture->flip);
    }
    return 0;
}

int main(void)
{
    struct mt_trust_table roots = {.count = 1, .min_model_version = 7};
    roots.keys[0].key_id = 3;
    roots.keys[0].active = 1;
    for (unsigned i = 0; i < 32; ++i) roots.keys[0].pubkey[i] = public_key[i];
    static const struct { uint32_t flip; int expected; } cases[] = {
        {UINT32_MAX, MS_OK},
        {3 * MS_SECTOR + 48, MS_EAUTH}, /* signature */
        {MS_SECTOR, MS_ETBLDIG},        /* signed directory */
        {2 * MS_SECTOR, MS_EDATA},      /* signed object's contents */
        {3 * MS_SECTOR + 200, MS_EFORMAT}, /* trailer padding */
    };
    struct ms_signed_scratch scratch;
    struct ms_volume result;
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        struct fixture fixture = {cases[i].flip};
        result.nobjects = 99;
        int rc = ms_verify_signed_volume(read_fixture, &fixture, 4, &roots, &scratch, &result);
        if (rc != cases[i].expected || result.nobjects != (rc == MS_OK ? 1u : 99u))
            return (int)i + 1;
    }
    struct fixture intact = {UINT32_MAX};
    roots.count = 0;
    result.nobjects = 99;
    if (ms_verify_signed_volume(read_fixture, &intact, 4, &roots, &scratch, &result) != MS_EAUTH ||
        result.nobjects != 99) return 6;
    return 0;
}
