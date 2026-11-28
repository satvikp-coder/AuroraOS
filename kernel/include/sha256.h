#ifndef AURORA_SHA256_H
#define AURORA_SHA256_H
#include <stddef.h>
#include <stdint.h>

/*
 * SHA-256 (FIPS 180-4), freestanding: no libc, no allocation, no global state. A digest is an UNKEYED integrity check: it detects accidental damage and
 * lets two copies of data be compared, but anyone who can change the data can compute a new digest. It is NOT authentication. See docs/kernel/model-asset-store.md.
 */
struct sha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t n; };
void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t len);
void sha256_final(struct sha256 *s, uint8_t out[32]);          /* the context must be re-initialised before reuse */
/* Compare two digests without an early exit (no timing signal about where they differ). Returns 1 if equal. */
int sha256_equal(const uint8_t a[32], const uint8_t b[32]);
/* RFC 2104 HMAC-SHA256 and RFC 2898 PBKDF2-HMAC-SHA256 (32-byte derived key, freestanding). */
void hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len, uint8_t out[32]);
void pbkdf2_hmac_sha256(const void *pw, size_t pw_len, const void *salt, size_t salt_len, uint32_t iterations, uint8_t out[32]);
#endif
