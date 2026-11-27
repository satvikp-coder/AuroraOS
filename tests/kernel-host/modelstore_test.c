/*
 * tests/kernel-host/modelstore_test.c - SHA-256 and the AURMODEL volume format (kernel/crypto/sha256.c, kernel/fs/modelstore.c), host, ASan/UBSan.
 * The oracle is independent: the test carries its OWN straightforward SHA-256 (whole padded message, full 64-word schedule) and builds volumes itself.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <modelstore.h>
#include <model_trust.h>
#include <sha256.h>

static int total_checks, fails;
static void check(const char *what, int ok) { total_checks++; if (!ok) { fails++; printf("  FAIL: %s\n", what); } }

static int mock_sig_verify(const uint8_t sig[64], const uint8_t *msg, size_t msg_len, const uint8_t pk[32])
{
    if (!sig || !msg || msg_len != 32 || !pk) return -1;
    for (size_t i = 0; i < 63; i++) if (sig[i] != (uint8_t)(msg[i % 32] ^ pk[i % 32] ^ 0x5Au)) return -1;
    return sig[63] == 0x11u ? 0 : -1;
}

/* ---- an independent reference SHA-256: pads the WHOLE message first, then walks it ---- */
static uint32_t rr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void ref_sha256(const uint8_t *m, size_t len, uint8_t out[32])
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t H[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    size_t padded = ((len + 8) / 64 + 1) * 64; uint8_t *p = calloc(padded, 1);
    memcpy(p, m, len); p[len] = 0x80; uint64_t bits = (uint64_t)len * 8; for (int i = 0; i < 8; i++) p[padded - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[64];
        for (int t = 0; t < 16; t++) w[t] = (uint32_t)p[off + 4 * t] << 24 | (uint32_t)p[off + 4 * t + 1] << 16 | (uint32_t)p[off + 4 * t + 2] << 8 | p[off + 4 * t + 3];
        for (int t = 16; t < 64; t++) w[t] = (rr(w[t - 2], 17) ^ rr(w[t - 2], 19) ^ (w[t - 2] >> 10)) + w[t - 7] + (rr(w[t - 15], 7) ^ rr(w[t - 15], 18) ^ (w[t - 15] >> 3)) + w[t - 16];
        uint32_t v[8]; memcpy(v, H, sizeof v);
        for (int t = 0; t < 64; t++) {
            uint32_t t1 = v[7] + (rr(v[4], 6) ^ rr(v[4], 11) ^ rr(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[t] + w[t];
            uint32_t t2 = (rr(v[0], 2) ^ rr(v[0], 13) ^ rr(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            memmove(v + 1, v, 7 * sizeof v[0]); v[4] += t1; v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) H[i] += v[i];
    }
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(H[i] >> 24); out[4 * i + 1] = (uint8_t)(H[i] >> 16); out[4 * i + 2] = (uint8_t)(H[i] >> 8); out[4 * i + 3] = (uint8_t)H[i]; }
    free(p);
}
static void hexs(const uint8_t *d, char *o) { for (int i = 0; i < 32; i++) sprintf(o + 2 * i, "%02x", d[i]); }
static void k_sha(const void *m, size_t n, uint8_t out[32]) { struct sha256 c; sha256_init(&c); sha256_update(&c, m, n); sha256_final(&c, out); }

static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

/* ---- volume builder ---- */
#define TOTAL 64
static uint8_t img[TOTAL * MS_SECTOR];
static void p32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void p64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
struct spec { const char *name; uint64_t size, first; uint8_t fill; };
static void seal(uint32_t n)           /* recompute the table digest and the header digest so that ONLY the semantic checks can object */
{
    k_sha(img + MS_SECTOR, n * MS_ENTRY, img + 64);
    k_sha(img, 96, img + 96);
}
static void build(const struct spec *s, uint32_t n)
{
    memset(img, 0, sizeof img);
    memcpy(img, "AURMODEL", 8); p32(img + 8, 1); p32(img + 12, 512); p64(img + 16, TOTAL); p32(img + 24, n); p32(img + 28, 1); p32(img + 32, (n * 128 + 511) / 512);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *e = img + MS_SECTOR + i * MS_ENTRY; strncpy((char *)e, s[i].name, 31);
        p64(e + 32, s[i].size); p64(e + 40, s[i].first); p64(e + 48, (s[i].size + 511) / 512); p32(e + 56, 1);
        if (s[i].first >= 2 && s[i].first * MS_SECTOR + s[i].size <= sizeof img) {          /* hostile specs (inside the header, past the end) get no data written, only a digest */
            uint8_t *data = img + s[i].first * MS_SECTOR; for (uint64_t b = 0; b < s[i].size; b++) data[b] = (uint8_t)(s[i].fill + b * 7);
            ref_sha256(data, (size_t)s[i].size, e + 64);
        } else memset(e + 64, 0xAB, 32);
    }
    seal(n);
}
static int rd(void *ctx, uint64_t sector, uint32_t count, uint8_t *buf) { (void)ctx; if (sector + count > TOTAL) return -1; memcpy(buf, img + sector * MS_SECTOR, count * MS_SECTOR); return 0; }
static struct ms_volume V;
static int parse(void) { return ms_parse(img, img + MS_SECTOR, MS_SECTOR, &V); }                    /* the table is sector 1 (up to 4 entries fit one sector) */
static int bad_rd(void *c, uint64_t s, uint32_t n, uint8_t *b) { (void)c; (void)s; (void)n; (void)b; return -1; }

int main(void)
{
    /* ---- SHA-256: published vectors ---- */
    char hx[65]; uint8_t d[32];
    k_sha("", 0, d); hexs(d, hx); check("SHA-256 of the empty message", !strcmp(hx, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    k_sha("abc", 3, d); hexs(d, hx); check("SHA-256 of \"abc\"", !strcmp(hx, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; k_sha(m448, strlen(m448), d); hexs(d, hx);
    check("SHA-256 of the 448-bit NIST message (two blocks)", !strcmp(hx, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    const char *m896 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"; k_sha(m896, strlen(m896), d); hexs(d, hx);
    check("SHA-256 of the 896-bit NIST message", !strcmp(hx, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
    { struct sha256 c; sha256_init(&c); uint8_t blk[1000]; memset(blk, 'a', sizeof blk); for (int i = 0; i < 1000; i++) sha256_update(&c, blk, sizeof blk); sha256_final(&c, d); hexs(d, hx);
      check("SHA-256 of one million 'a'", !strcmp(hx, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")); }
    /* ---- against the independent implementation: every length around the padding boundaries and random chunking ---- */
    { int bad = 0; uint8_t *buf = malloc(700); for (int i = 0; i < 700; i++) buf[i] = (uint8_t)rnd();
      for (size_t n = 0; n <= 300; n++) { uint8_t a[32], b[32]; ref_sha256(buf, n, a); k_sha(buf, n, b); if (memcmp(a, b, 32)) bad++; }
      for (int t = 0; t < 400; t++) { size_t n = rnd() % 700; uint8_t a[32], b[32]; ref_sha256(buf, n, a);
          struct sha256 c; sha256_init(&c); size_t off = 0; while (off < n) { size_t k = 1 + rnd() % 130; if (k > n - off) k = n - off; sha256_update(&c, buf + off, k); off += k; } sha256_final(&c, b); if (memcmp(a, b, 32)) bad++; }
      check("every length 0..300 and 400 random messages in random chunks match the independent implementation", bad == 0); free(buf); }
    { uint8_t a[32], b[32]; memset(a, 7, 32); memcpy(b, a, 32); check("digest compare: equal", sha256_equal(a, b)); b[31] ^= 1; check("digest compare: one bit differs in the last byte", !sha256_equal(a, b)); b[31] ^= 1; b[0] ^= 0x80; check("digest compare: one bit differs in the first byte", !sha256_equal(a, b)); }

    /* ---- a valid volume ---- */
    const struct spec S3[3] = { { "tokenizer.bin", 1, 4, 11 }, { "weights.q8", 5000, 6, 22 }, { "config.json", 513, 20, 33 } };
    build(S3, 3);
    V.nobjects = 3; int rc = parse();
    check("a valid volume with three objects (1 byte, 5000 bytes, 513 bytes) parses", rc == MS_OK && V.nobjects == 3 && V.obj[1].size == 5000 && V.obj[1].sectors == 10 && !strcmp(V.obj[2].name, "config.json"));
    { int ok = 1; for (uint32_t i = 0; i < 3; i++) if (ms_verify_object(&V, i, rd, 0) != MS_OK) ok = 0; check("every object verifies against its recorded digest", ok); }

    /* ---- EVERY single-byte change of the header sector and of the table sector is rejected ---- */
    { int survived = 0, tried = 0;
      for (size_t i = 0; i < 2 * MS_SECTOR; i++) {
          uint8_t keep = img[i]; const uint8_t flips[3] = { 0x01, 0x80, 0xFF };
          for (int f = 0; f < 3; f++) { img[i] = keep ^ flips[f]; V.nobjects = 3; tried++; if (ms_parse(img, img + MS_SECTOR, MS_SECTOR, &V) == MS_OK) survived++; img[i] = keep; }
      }
      printf("    (%d single-byte mutations of the header and table; %d accepted)\n", tried, survived);
      check("every single-byte change anywhere in the header sector or the table sector is rejected (0 accepted)", survived == 0 && tried == 3 * 2 * MS_SECTOR); }
    /* ---- every single-byte change inside an object's sectors, including the zero tail, is detected ---- */
    { int survived = 0, tried = 0; V.nobjects = 3; ms_parse(img, img + MS_SECTOR, MS_SECTOR, &V);
      for (uint32_t o = 0; o < 3; o++)
          for (uint64_t b = 0; b < V.obj[o].sectors * MS_SECTOR; b++) {
              uint8_t *p = img + V.obj[o].first_sector * MS_SECTOR + b; uint8_t keep = *p; *p = keep ^ 0x5A; tried++;
              if (ms_verify_object(&V, o, rd, 0) == MS_OK) { survived++; }
              *p = keep;
          }
      printf("    (%d single-byte mutations of object data; %d accepted)\n", tried, survived);
      check("every single-byte change in an object's data or its zero tail is detected (0 accepted)", survived == 0); }
    check("a failed read (device error) is reported as an I/O error, not as success or as corruption", ms_verify_object(&V, 1, bad_rd, 0) == MS_EIO);

    /* ---- hostile but CORRECTLY SEALED volumes: only the semantic checks can reject these ---- */
    struct { const char *what; struct spec s[3]; uint32_t n; int want; } H[] = {
        { "two objects whose extents overlap", { { "a", 600, 4, 1 }, { "b", 600, 5, 2 } }, 2, MS_EEXTENT },
        { "an object inside the header and table area", { { "a", 100, 1, 1 } }, 1, MS_EEXTENT },
        { "an object that ends past the volume", { { "a", 5000, 60, 1 } }, 1, MS_EEXTENT },
        { "an object of size zero", { { "a", 0, 4, 1 } }, 1, MS_EEXTENT },
        { "duplicate names", { { "same", 10, 4, 1 }, { "same", 10, 8, 2 } }, 2, MS_EDUP },
        { "a name with a space", { { "bad name", 10, 4, 1 } }, 1, MS_ENAME },
        { "an empty name", { { "", 10, 4, 1 } }, 1, MS_ENAME },
        { "a name with a slash", { { "dir/file", 10, 4, 1 } }, 1, MS_ENAME },
    };
    for (size_t i = 0; i < sizeof H / sizeof H[0]; i++) {
        build(H[i].s, H[i].n);
        V.nobjects = H[i].n; char w[160]; snprintf(w, sizeof w, "%s is rejected with the right reason", H[i].what);
        check(w, parse() == H[i].want);
    }
    /* fields forced to extreme values and re-sealed */
    build(S3, 3);
    { uint8_t save[MS_SECTOR]; memcpy(save, img + MS_SECTOR, MS_SECTOR);
      p64(img + MS_SECTOR + 32, 1ULL << 63); seal(3); V.nobjects = 3; check("a size of 2^63 is rejected (no wraparound into a small number)", parse() == MS_EEXTENT);
      memcpy(img + MS_SECTOR, save, MS_SECTOR); p64(img + MS_SECTOR + 32, ~0ULL); p64(img + MS_SECTOR + 48, 0); seal(3); check("a size of 2^64-1 with sector count 0 (the rounding-up addition wraps to 0) is rejected by the size bound", parse() == MS_EEXTENT);
      memcpy(img + MS_SECTOR, save, MS_SECTOR); p64(img + MS_SECTOR + 40, ~0ULL); seal(3); check("a first sector of 2^64-1 is rejected", parse() == MS_EEXTENT);
      memcpy(img + MS_SECTOR, save, MS_SECTOR); p64(img + MS_SECTOR + 48, 3); seal(3); check("a sector count that does not match the size is rejected", parse() == MS_EEXTENT);
      memcpy(img + MS_SECTOR, save, MS_SECTOR); p64(img + MS_SECTOR + 40, 1ULL << 63); p64(img + MS_SECTOR + 48, 1ULL << 63); p64(img + MS_SECTOR + 32, (1ULL << 63) * 512); seal(3); check("an extent whose first+count wraps 2^64 is rejected", parse() == MS_EEXTENT);
      memcpy(img + MS_SECTOR, save, MS_SECTOR); img[MS_SECTOR + 60] = 1; seal(3); check("a non-zero flags field is rejected", parse() == MS_EFORMAT);
      memcpy(img + MS_SECTOR, save, MS_SECTOR); img[MS_SECTOR + 100] = 1; seal(3); check("non-zero reserved bytes in an entry are rejected", parse() == MS_EFORMAT);
      memcpy(img + MS_SECTOR, save, MS_SECTOR); seal(3); }
    { uint8_t hdr[MS_SECTOR]; memcpy(hdr, img, MS_SECTOR);
      p32(img + 24, 17); k_sha(img, 96, img + 96); V.nobjects = 17; check("17 objects exceeds the limit of 16", parse() == MS_ECOUNT);
      memcpy(img, hdr, MS_SECTOR); p64(img + 16, 1ULL << 41); k_sha(img, 96, img + 96); check("a volume of 2^41 sectors is rejected", ms_parse(img, img + MS_SECTOR, MS_SECTOR, &V) == MS_EGEOM);
      memcpy(img, hdr, MS_SECTOR); p32(img + 8, 2); k_sha(img, 96, img + 96); check("an unknown version is rejected as a version error", ms_parse(img, img + MS_SECTOR, MS_SECTOR, &V) == MS_EVERSION);
      memcpy(img, hdr, MS_SECTOR); img[0] = 'X'; check("a wrong magic is rejected as a magic error", ms_parse(img, img + MS_SECTOR, MS_SECTOR, &V) == MS_EMAGIC);
      memcpy(img, hdr, MS_SECTOR);
      check("a table shorter than its sectors is refused, not read past", ms_parse(img, img + MS_SECTOR, 100, &V) == MS_EARG);
      check("NULL arguments are refused", ms_parse(0, img, MS_SECTOR, &V) == MS_EARG && ms_parse(img, 0, MS_SECTOR, &V) == MS_EARG && ms_parse(img, img, MS_SECTOR, 0) == MS_EARG && ms_verify_object(0, 0, rd, 0) == MS_EARG); }
    check("an object index past the end is refused", ms_verify_object(&V, 99, rd, 0) == MS_EARG);

    /* ---- the limit, asserted: a digest is not authentication ---- */
    build(S3, 3);
    { V.nobjects = 3; ms_parse(img, img + MS_SECTOR, MS_SECTOR, &V);
      uint8_t *data = img + V.obj[1].first_sector * MS_SECTOR; data[0] ^= 0xFF; uint8_t *e = img + MS_SECTOR + 1 * MS_ENTRY; ref_sha256(data, 5000, e + 64); seal(3);
      V.nobjects = 3; int p = ms_parse(img, img + MS_SECTOR, MS_SECTOR, &V);
      check("KNOWN LIMIT: weights changed AND every digest recomputed still verifies, because an unkeyed digest authenticates nothing (a signature is required)", p == MS_OK && ms_verify_object(&V, 1, rd, 0) == MS_OK); }

    /* ---- v2 signature envelope defeats the v1 digest-recompute attack ---- */
    {
        build(S3, 3);
        uint8_t pk[32]; for (size_t i = 0; i < 32; i++) pk[i] = (uint8_t)(0x20u + i);
        struct mt_trust_table trust; memset(&trust, 0, sizeof trust);
        trust.count = 1; trust.min_model_version = 1;
        trust.keys[0].key_id = 7; memcpy(trust.keys[0].pubkey, pk, 32); trust.keys[0].active = 1;
        uint8_t tr[MT_TRAILER_BYTES]; memset(tr, 0, sizeof tr);
        tr[0] = 'A'; tr[1] = 'U'; tr[2] = 'R'; tr[3] = 'S';
        p32(tr + 4, MT_TRAILER_VERSION); p32(tr + 8, 7); p32(tr + 12, 1);
        memcpy(tr + 16, img + 96, 32);
        for (size_t i = 0; i < 63; i++) tr[48 + i] = (uint8_t)(img[96 + (i % 32)] ^ pk[i % 32] ^ 0x5Au);
        tr[48 + 63] = 0x11u;
        struct mt_trailer parsed;
        check("v2 trailer authenticates unmodified sealed header", ms_verify_v2_trailer(img, &trust, tr, sizeof tr, mock_sig_verify, &parsed) == MT_AUTHENTICATED && parsed.key_id == 7);
        /* Attacker flips weight byte and recomputes all SHA-256 digests */
        V.nobjects = 3; ms_parse(img, img + MS_SECTOR, MS_SECTOR, &V);
        uint8_t *data = img + V.obj[1].first_sector * MS_SECTOR; data[0] ^= 0xFF;
        ref_sha256(data, 5000, img + MS_SECTOR + 1 * MS_ENTRY + 64); seal(3);
        check("v2 trailer REJECTS volume when weights change and digests are recomputed", ms_verify_v2_trailer(img, &trust, tr, sizeof tr, mock_sig_verify, 0) == MT_E_DIGEST);
    }

    printf("modelstore: %d passed, %d failed\n", total_checks - fails, fails);
    return fails != 0;
}
