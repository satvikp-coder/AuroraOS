/* tests/tools/mkmodel.c: host tool that writes an AURMODEL v1 model volume image (format: kernel/include/modelstore.h) for QEMU tests.
 *
 *   mkmodel [--corrupt-object N] [--corrupt-header] out.img name[:kind]=file [name[:kind]=file ...]
 *
 * kind is the table entry's u32 kind (decimal or 0x hex); it defaults to 0, so a spec without ":kind" produces exactly the bytes it always did.
 * (Names may not contain ':', so the suffix is unambiguous.) Kind 1 is the int8 matvec tensor of tests/programs/matvec_core.h.
 * Objects are laid out back to back after the table, each starting on a sector boundary, with zero tails. Digests are SHA-256 (kernel/crypto/sha256.c).
 * An uncorrupted image is re-checked with ms_parse() before it is written. The corruption options exist only so tests can build bad images:
 *   --corrupt-object N   flip one bit in the middle of object N's data (header and table stay valid: only the object digest fails)
 *   --corrupt-header     flip one bit in the header's own digest (byte 100); the magic is untouched, so the device is still identified as a model volume
 * Build: cc -std=c11 -Wall -Wextra -O1 -I kernel/include -o mkmodel tests/tools/mkmodel.c kernel/fs/modelstore.c kernel/crypto/sha256.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <modelstore.h>
#include <sha256.h>

static void p32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void p64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void digest(const void *d, size_t n, uint8_t out[32]) { struct sha256 c; sha256_init(&c); sha256_update(&c, d, n); sha256_final(&c, out); }
static int usage(void) { fprintf(stderr, "usage: mkmodel [--corrupt-object N] [--corrupt-header] out.img name[:kind]=file [name[:kind]=file ...]\n"); return 2; }

int main(int argc, char **argv)
{
    long corrupt_obj = -1; int corrupt_hdr = 0, a = 1;
    for (; a < argc && strncmp(argv[a], "--", 2) == 0; a++) {
        if (strcmp(argv[a], "--corrupt-object") == 0 && a + 1 < argc) corrupt_obj = strtol(argv[++a], NULL, 10);
        else if (strcmp(argv[a], "--corrupt-header") == 0) corrupt_hdr = 1;
        else return usage();
    }
    if (a >= argc) return usage();
    const char *out = argv[a++];
    uint32_t n = (uint32_t)(argc - a);
    if (n > MS_MAX_OBJECTS) { fprintf(stderr, "mkmodel: at most %u objects\n", MS_MAX_OBJECTS); return 1; }
    if (corrupt_obj >= (long)n) { fprintf(stderr, "mkmodel: --corrupt-object %ld: no such object\n", corrupt_obj); return 1; }

    char names[MS_MAX_OBJECTS][MS_NAME_MAX + 1]; uint8_t *data[MS_MAX_OBJECTS]; uint64_t size[MS_MAX_OBJECTS], first[MS_MAX_OBJECTS], secs[MS_MAX_OBJECTS];
    uint32_t kinds[MS_MAX_OBJECTS];
    uint32_t tn = (n * MS_ENTRY + MS_SECTOR - 1) / MS_SECTOR;
    uint64_t next = 1 + tn;
    for (uint32_t i = 0; i < n; i++) {
        const char *arg = argv[a + i], *eq = strchr(arg, '=');
        const char *colon = eq ? memchr(arg, ':', (size_t)(eq - arg)) : NULL, *nend = colon ? colon : eq;
        if (!eq || nend == arg || (size_t)(nend - arg) > MS_NAME_MAX) { fprintf(stderr, "mkmodel: bad object spec '%s' (want name[:kind]=file, name 1..%u chars)\n", arg, MS_NAME_MAX); return 1; }
        kinds[i] = 0;
        if (colon) {
            char *end = NULL; unsigned long k = strtoul(colon + 1, &end, 0);
            if (end == colon + 1 || end != eq || k > 0xFFFFFFFFul) { fprintf(stderr, "mkmodel: bad kind in '%s'\n", arg); return 1; }
            kinds[i] = (uint32_t)k;
        }
        memset(names[i], 0, sizeof names[i]); memcpy(names[i], arg, (size_t)(nend - arg));
        FILE *f = fopen(eq + 1, "rb");
        if (!f) { perror(eq + 1); return 1; }
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        if (len <= 0) { fprintf(stderr, "mkmodel: %s: empty objects are not allowed\n", eq + 1); fclose(f); return 1; }
        data[i] = malloc((size_t)len);
        if (!data[i] || fread(data[i], 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "mkmodel: %s: read failed\n", eq + 1); fclose(f); return 1; }
        fclose(f);
        size[i] = (uint64_t)len; secs[i] = (size[i] + MS_SECTOR - 1) / MS_SECTOR; first[i] = next; next += secs[i];
    }
    uint64_t total = next < 2 ? 2 : next;                       /* the format requires at least two sectors */
    uint8_t *img = calloc((size_t)total, MS_SECTOR);
    if (!img) { fprintf(stderr, "mkmodel: out of memory\n"); return 1; }
    uint8_t *h = img, *t = img + MS_SECTOR;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *e = t + (size_t)i * MS_ENTRY;
        memcpy(e, names[i], MS_NAME_MAX + 1);
        p64(e + 32, size[i]); p64(e + 40, first[i]); p64(e + 48, secs[i]); p32(e + 56, kinds[i]); p32(e + 60, 0);
        digest(data[i], (size_t)size[i], e + 64);
        memcpy(img + first[i] * MS_SECTOR, data[i], (size_t)size[i]);
    }
    memcpy(h, "AURMODEL", 8);
    p32(h + 8, 1); p32(h + 12, MS_SECTOR); p64(h + 16, total); p32(h + 24, n); p32(h + 28, 1); p32(h + 32, tn); p32(h + 36, 0);
    digest(t, (size_t)n * MS_ENTRY, h + 64);
    digest(h, 96, h + 96);

    struct ms_volume v;
    int rc = ms_parse(h, t, (size_t)tn * MS_SECTOR, &v);
    if (rc != MS_OK) { fprintf(stderr, "mkmodel: internal error: the image does not parse (%d)\n", rc); return 1; }
    if (corrupt_obj >= 0) img[first[corrupt_obj] * MS_SECTOR + size[corrupt_obj] / 2] ^= 0x01;
    if (corrupt_hdr) h[100] ^= 0x01;

    FILE *f = fopen(out, "wb");
    if (!f || fwrite(img, MS_SECTOR, (size_t)total, f) != (size_t)total || fclose(f) != 0) { perror(out); return 1; }
    printf("mkmodel: %s: %u objects, %llu sectors%s%s\n", out, n, (unsigned long long)total, corrupt_obj >= 0 ? " (object corrupted)" : "", corrupt_hdr ? " (header corrupted)" : "");
    return 0;
}
