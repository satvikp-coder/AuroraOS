/* tests/kernel-host/model_trust_test.c - host tests for kernel/include/model_trust.h */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <model_trust.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Deterministic test verifier: expects sig[i] == (msg[i % 48] ^ pubkey[i % 32] ^ 0x5A) for i < 63, sig[63] == 0x11. */
static int mock_verify(const uint8_t sig[64], const uint8_t *msg, size_t msg_len, const uint8_t pubkey[32])
{
    if (!sig || !msg || msg_len != 48 || !pubkey) return -1;
    for (size_t i = 0; i < 63; i++) {
        if (sig[i] != (uint8_t)(msg[i % 48] ^ pubkey[i % 32] ^ 0x5Au)) return -1;
    }
    return sig[63] == 0x11u ? 0 : -1;
}

static void build_valid_trailer(uint8_t out[MT_TRAILER_BYTES],
                                uint32_t key_id,
                                uint32_t model_ver,
                                const uint8_t digest[32],
                                const uint8_t pubkey[32])
{
    memset(out, 0, MT_TRAILER_BYTES);
    out[0] = 'A'; out[1] = 'U'; out[2] = 'R'; out[3] = 'S';
    put_le32(out + 4, MT_TRAILER_VERSION);
    put_le32(out + 8, key_id);
    put_le32(out + 12, model_ver);
    memcpy(out + 16, digest, 32);
    for (size_t i = 0; i < 63; i++) out[48 + i] = (uint8_t)(out[i % 48] ^ pubkey[i % 32] ^ 0x5Au);
    out[48 + 63] = 0x11u;
    put_le32(out + 112, 0);
}

int main(void)
{
    uint8_t pk1[32], pk2[32], digest[32];
    for (size_t i = 0; i < 32; i++) {
        pk1[i] = (uint8_t)(0x10u + i);
        pk2[i] = (uint8_t)(0x40u + i);
        digest[i] = (uint8_t)(0x80u + i);
    }

    struct mt_trust_table trust;
    memset(&trust, 0, sizeof(trust));
    trust.count = 2;
    trust.min_model_version = 3;
    trust.keys[0].key_id = 101; memcpy(trust.keys[0].pubkey, pk1, 32); trust.keys[0].active = 1;
    trust.keys[1].key_id = 102; memcpy(trust.keys[1].pubkey, pk2, 32); trust.keys[1].active = 0; /* revoked */

    uint8_t tr[MT_TRAILER_BYTES];
    struct mt_trailer parsed;
    build_valid_trailer(tr, 101, 3, digest, pk1);

    /* Valid at min_model_version */
    CHECK("valid trailer at min_model_version accepted",
          mt_verify_envelope(&trust, digest, tr, MT_TRAILER_BYTES, mock_verify, &parsed) == MT_AUTHENTICATED &&
          parsed.key_id == 101 && parsed.model_version == 3);

    uint8_t edited[MT_TRAILER_BYTES];
    memcpy(edited, tr, sizeof edited);
    put_le32(edited + 12, 4);
    parsed.model_version = 17;
    CHECK("changed model version fails signature verification and leaves output unchanged",
          mt_verify_envelope(&trust, digest, edited, sizeof edited, mock_verify, &parsed) == MT_E_BAD_SIG &&
          parsed.model_version == 17);
    trust.keys[1].active = 1;
    memcpy(trust.keys[1].pubkey, pk1, sizeof pk1);
    memcpy(edited, tr, sizeof edited);
    put_le32(edited + 8, 102);
    CHECK("key ID is bound even when trust entries share the same public key",
          mt_verify_envelope(&trust, digest, edited, sizeof edited, mock_verify, 0) == MT_E_BAD_SIG);
    trust.keys[1].active = 0;
    memcpy(trust.keys[1].pubkey, pk2, sizeof pk2);

    /* Null verifier fails closed */
    CHECK("null verifier fails closed",
          mt_verify_envelope(&trust, digest, tr, MT_TRAILER_BYTES, 0, 0) == MT_E_NO_VERIFIER);

    /* Truncated / oversized / bad magic / bad version / non-zero reserved */
    CHECK("truncated trailer rejected", mt_verify_envelope(&trust, digest, tr, MT_TRAILER_BYTES - 1, mock_verify, 0) == MT_E_TRUNC);
    CHECK("oversized trailer rejected", mt_verify_envelope(&trust, digest, tr, MT_TRAILER_BYTES + 1, mock_verify, 0) == MT_E_FORMAT);

    uint8_t bad[MT_TRAILER_BYTES];
    memcpy(bad, tr, MT_TRAILER_BYTES); bad[0] = 'X';
    CHECK("bad magic rejected", mt_verify_envelope(&trust, digest, bad, MT_TRAILER_BYTES, mock_verify, 0) == MT_E_FORMAT);

    memcpy(bad, tr, MT_TRAILER_BYTES); put_le32(bad + 4, 1);
    CHECK("legacy digest-only trailer version rejected", mt_verify_envelope(&trust, digest, bad, MT_TRAILER_BYTES, mock_verify, 0) == MT_E_VERSION);

    memcpy(bad, tr, MT_TRAILER_BYTES); put_le32(bad + 112, 1);
    CHECK("non-zero reserved rejected", mt_verify_envelope(&trust, digest, bad, MT_TRAILER_BYTES, mock_verify, 0) == MT_E_FORMAT);

    /* Downgrade / rollback */
    build_valid_trailer(bad, 101, 2, digest, pk1);
    CHECK("downgrade model_version < min rejected", mt_verify_envelope(&trust, digest, bad, MT_TRAILER_BYTES, mock_verify, 0) == MT_E_ROLLBACK);

    /* Header digest mismatch */
    uint8_t other_digest[32];
    memcpy(other_digest, digest, 32); other_digest[0] ^= 1;
    CHECK("digest mismatch rejected", mt_verify_envelope(&trust, other_digest, tr, MT_TRAILER_BYTES, mock_verify, 0) == MT_E_DIGEST);

    /* Unknown and revoked keys */
    build_valid_trailer(bad, 999, 4, digest, pk1);
    CHECK("unknown key_id rejected", mt_verify_envelope(&trust, digest, bad, MT_TRAILER_BYTES, mock_verify, 0) == MT_E_UNKNOWN_KEY);

    build_valid_trailer(bad, 102, 4, digest, pk2);
    CHECK("revoked key_id rejected", mt_verify_envelope(&trust, digest, bad, MT_TRAILER_BYTES, mock_verify, 0) == MT_E_REVOKED_KEY);

    /* Malformed / flipped / all-zero / non-canonical S signatures */
    memcpy(bad, tr, MT_TRAILER_BYTES); bad[48 + 5] ^= 1;
    CHECK("single-bit sig flip rejected", mt_verify_envelope(&trust, digest, bad, MT_TRAILER_BYTES, mock_verify, 0) == MT_E_BAD_SIG);

    memcpy(bad, tr, MT_TRAILER_BYTES); memset(bad + 48, 0, 32);
    CHECK("all-zero R rejected", mt_verify_envelope(&trust, digest, bad, MT_TRAILER_BYTES, mock_verify, 0) == MT_E_BAD_SIG);

    memcpy(bad, tr, MT_TRAILER_BYTES); memset(bad + 80, 0, 32);
    CHECK("all-zero S rejected", mt_verify_envelope(&trust, digest, bad, MT_TRAILER_BYTES, mock_verify, 0) == MT_E_BAD_SIG);

    memcpy(bad, tr, MT_TRAILER_BYTES); bad[48 + 63] |= 0x80u;
    CHECK("non-canonical high S bit rejected", mt_verify_envelope(&trust, digest, bad, MT_TRAILER_BYTES, mock_verify, 0) == MT_E_BAD_SIG);

    printf("model_trust: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
