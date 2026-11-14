/*
 * tests/kernel-host/fwcfg_test.c - host tests for kernel/drivers/fwcfg_proto.c (fw_cfg directory, signature, DMA descriptor,
 * ramfb configuration). A fake fw_cfg device serves byte streams; no hardware is touched.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fwcfg.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; printf("  PASS: %s\n", name); } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

struct fake { uint16_t key; uint32_t off; uint8_t *items[0x4000]; uint32_t len[0x4000]; uint64_t reads; };
static void f_select(void *c, uint16_t k) { struct fake *f = c; f->key = k; f->off = 0; }
static void f_read(void *c, void *buf, uint32_t n)
{
    struct fake *f = c; uint8_t *o = buf;
    for (uint32_t i = 0; i < n; i++) { o[i] = (f->items[f->key] && f->off < f->len[f->key]) ? f->items[f->key][f->off] : 0; f->off++; f->reads++; }
}
static const struct fwcfg_ops OPS = { f_select, f_read };

struct ent { const char *name; uint32_t size; uint16_t sel; };
/* Build a directory of `n` entries (names may be forced to 56 non-NUL bytes by the caller via raw_name). */
static void set_dir(struct fake *f, const struct ent *e, uint32_t n, uint32_t claimed_count)
{
    free(f->items[FWCFG_KEY_FILE_DIR]);
    uint8_t *d = calloc(1, 4 + (size_t)n * 64 + 8);
    fwcfg_put_be32(d, claimed_count);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *p = d + 4 + i * 64;
        fwcfg_put_be32(p, e[i].size); p[4] = (uint8_t)(e[i].sel >> 8); p[5] = (uint8_t)e[i].sel;
        strncpy((char *)p + 8, e[i].name, 55);
    }
    f->items[FWCFG_KEY_FILE_DIR] = d; f->len[FWCFG_KEY_FILE_DIR] = 4 + n * 64;
}
static void set_item(struct fake *f, uint16_t k, const void *data, uint32_t n)
{ free(f->items[k]); f->items[k] = malloc(n ? n : 1); memcpy(f->items[k], data, n); f->len[k] = n; }

int main(void)
{
    static struct fake F; struct fwcfg_file out;

    printf("endianness helpers\n");
    { const uint8_t b[8] = {1,2,3,4,5,6,7,8}; uint8_t o[8];
      CHECK("be16", fwcfg_get_be16(b) == 0x0102); CHECK("be32", fwcfg_get_be32(b) == 0x01020304);
      fwcfg_put_be64(o, 0x0102030405060708ULL); CHECK("put_be64 bytes", memcmp(o, b, 8) == 0);
      fwcfg_put_be32(o, 0xA1B2C3D4u); CHECK("put_be32 bytes", o[0] == 0xA1 && o[3] == 0xD4); }

    printf("signature and features\n");
    { uint32_t feat = 0;
      set_item(&F, FWCFG_KEY_SIGNATURE, "QEMU", 4);
      const uint8_t id_dma[4] = { 3, 0, 0, 0 }, id_nodma[4] = { 1, 0, 0, 0 }, id_be[4] = { 0, 0, 0, 3 };
      set_item(&F, FWCFG_KEY_ID, id_dma, 4);
      CHECK("QEMU + DMA accepted", fwcfg_check_device(&OPS, &F, 1, &feat) == FWCFG_OK && feat == 3);
      set_item(&F, FWCFG_KEY_ID, id_nodma, 4);
      CHECK("no DMA bit rejected when DMA is required", fwcfg_check_device(&OPS, &F, 1, 0) == FWCFG_ENODMA);
      CHECK("no DMA bit accepted when not required", fwcfg_check_device(&OPS, &F, 0, 0) == FWCFG_OK);
      set_item(&F, FWCFG_KEY_ID, id_be, 4);   /* the bitmap is LITTLE-endian: bytes 00 00 00 03 mean 0x03000000, i.e. no DMA bit */
      CHECK("feature bitmap is read little-endian", fwcfg_check_device(&OPS, &F, 1, &feat) == FWCFG_ENODMA && feat == 0x03000000u);
      set_item(&F, FWCFG_KEY_SIGNATURE, "QEMX", 4);
      CHECK("wrong signature rejected", fwcfg_check_device(&OPS, &F, 0, 0) == FWCFG_ESIG);
      set_item(&F, FWCFG_KEY_SIGNATURE, "\0\0\0\0", 4);
      CHECK("blank bus (all zero) rejected", fwcfg_check_device(&OPS, &F, 0, 0) == FWCFG_ESIG);
      CHECK("null ops rejected", fwcfg_check_device(0, &F, 0, 0) == FWCFG_EARG); }

    printf("file directory\n");
    { struct ent e[] = { { "etc/acpi/tables", 100, 0x20 }, { "etc/ramfb", 28, 0x22 }, { "bootorder", 5, 0x21 } };
      set_dir(&F, e, 3, 3);
      CHECK("finds etc/ramfb", fwcfg_find_file(&OPS, &F, "etc/ramfb", &out) == FWCFG_OK && out.size == 28 && out.select == 0x22);
      CHECK("finds first entry", fwcfg_find_file(&OPS, &F, "etc/acpi/tables", &out) == FWCFG_OK && out.select == 0x20);
      CHECK("missing file", fwcfg_find_file(&OPS, &F, "etc/nothing", &out) == FWCFG_ENOFILE);
      CHECK("prefix is not a match", fwcfg_find_file(&OPS, &F, "etc/ramf", &out) == FWCFG_ENOFILE);
      CHECK("longer name is not a match", fwcfg_find_file(&OPS, &F, "etc/ramfb2", &out) == FWCFG_ENOFILE);
      CHECK("empty name rejected", fwcfg_find_file(&OPS, &F, "", &out) == FWCFG_EARG);
      CHECK("name longer than the field rejected", fwcfg_find_file(&OPS, &F, "0123456789012345678901234567890123456789012345678901234567890", &out) == FWCFG_EARG);
      CHECK("null out rejected", fwcfg_find_file(&OPS, &F, "etc/ramfb", 0) == FWCFG_EARG); }
    { struct ent e[] = { { "etc/ramfb", 28, 0x22 } };
      set_dir(&F, e, 1, 0);
      CHECK("empty directory: not found", fwcfg_find_file(&OPS, &F, "etc/ramfb", &out) == FWCFG_ENOFILE);
      set_dir(&F, e, 1, FWCFG_MAX_FILES + 1);
      F.reads = 0;
      CHECK("count over the bound is refused", fwcfg_find_file(&OPS, &F, "etc/ramfb", &out) == FWCFG_EDIR);
      CHECK("...after reading only the 4 byte header", F.reads == 4);
      set_dir(&F, e, 1, 0xFFFFFFFFu);
      CHECK("count 0xFFFFFFFF refused", fwcfg_find_file(&OPS, &F, "etc/ramfb", &out) == FWCFG_EDIR);
      struct ent dup[] = { { "etc/ramfb", 28, 0x22 }, { "x", 1, 0x23 }, { "etc/ramfb", 28, 0x24 } };
      set_dir(&F, dup, 3, 3);
      CHECK("duplicate name is ambiguous: refused", fwcfg_find_file(&OPS, &F, "etc/ramfb", &out) == FWCFG_EDIR);
      CHECK("a unique name in the same directory still works", fwcfg_find_file(&OPS, &F, "x", &out) == FWCFG_OK && out.select == 0x23);
      struct ent low[] = { { "etc/ramfb", 28, 0x05 } };
      set_dir(&F, low, 1, 1);
      CHECK("selector below FW_CFG_FILE_FIRST refused", fwcfg_find_file(&OPS, &F, "etc/ramfb", &out) == FWCFG_EDIR); }
    { /* a name with no NUL in its 56 bytes poisons the directory, even when it is not the one we look for */
      struct ent e[] = { { "a", 1, 0x20 }, { "etc/ramfb", 28, 0x22 } };
      set_dir(&F, e, 2, 2);
      memset(F.items[FWCFG_KEY_FILE_DIR] + 4 + 8, 'A', 56);
      CHECK("unterminated name refused", fwcfg_find_file(&OPS, &F, "etc/ramfb", &out) == FWCFG_EDIR);
      F.len[FWCFG_KEY_FILE_DIR] = 4;   /* truncated directory: reads past the end return zeros (device behaviour); names are empty, so not found */
      CHECK("truncated directory never over-reads", fwcfg_find_file(&OPS, &F, "etc/ramfb", &out) == FWCFG_ENOFILE); }
    { /* maximum legal directory still parses and is bounded in work */
      static struct ent big[FWCFG_MAX_FILES]; static char names[FWCFG_MAX_FILES][16];
      for (uint32_t i = 0; i < FWCFG_MAX_FILES; i++) { snprintf(names[i], 16, "f/%u", i); big[i].name = names[i]; big[i].size = i; big[i].sel = (uint16_t)(0x20 + i); }
      set_dir(&F, big, FWCFG_MAX_FILES, FWCFG_MAX_FILES); F.reads = 0;
      CHECK("1024 entries: last one found", fwcfg_find_file(&OPS, &F, "f/1023", &out) == FWCFG_OK && out.select == 0x20 + 1023);
      CHECK("work is bounded by 4 + 64*count bytes", F.reads == 4 + 64ull * FWCFG_MAX_FILES); }

    printf("DMA descriptor\n");
    { struct fwcfg_dma d; fwcfg_dma_fill(&d, 0x22, FWCFG_DMA_WRITE, 28, 0x4FD00000ULL);
      /* control = WRITE(0x10) | SELECT(0x08) | key<<16 = 0x00220018, big-endian on the wire */
      CHECK("control word big-endian with select", d.b[0] == 0x00 && d.b[1] == 0x22 && d.b[2] == 0x00 && d.b[3] == 0x18);
      CHECK("length big-endian", d.b[4] == 0 && d.b[5] == 0 && d.b[6] == 0 && d.b[7] == 28);
      CHECK("address big-endian", d.b[8] == 0 && d.b[9] == 0 && d.b[10] == 0 && d.b[11] == 0 && d.b[12] == 0x4F && d.b[13] == 0xD0 && d.b[14] == 0 && d.b[15] == 0);
      CHECK("size is exactly 16", sizeof d == 16);
      uint8_t pend[4] = { 0, 0, 0, 0x18 }, done[4] = { 0, 0, 0, 0 }, err[4] = { 0, 0, 0, 0x19 }, errhi[4] = { 0, 0x22, 0, 1 };
      CHECK("control still set: pending", fwcfg_dma_status(pend) == 0);
      CHECK("control zero: done", fwcfg_dma_status(done) == 1);
      CHECK("error bit: error even with other bits set", fwcfg_dma_status(err) == FWCFG_EDMA);
      CHECK("error bit with select field set", fwcfg_dma_status(errhi) == FWCFG_EDMA); }

    printf("ramfb configuration\n");
    { uint8_t c[RAMFB_CFG_SIZE];
      CHECK("size is 28", RAMFB_CFG_SIZE == 28);
      CHECK("XR24 fourcc", RAMFB_FOURCC_XR24 == ('X' | ('R' << 8) | ('2' << 16) | ((uint32_t)'4' << 24)));
      CHECK("builds", ramfb_build_cfg(c, 0x4FD00000ULL, RAMFB_FOURCC_XR24, 1024, 768, 4096) == FWCFG_OK);
      CHECK("addr big-endian", fwcfg_get_be32(c) == 0 && fwcfg_get_be32(c + 4) == 0x4FD00000u);
      CHECK("fourcc big-endian bytes are 'X','R','2','4' reversed on the wire as 0x34325258", fwcfg_get_be32(c + 8) == 0x34325258u);
      CHECK("flags zero", fwcfg_get_be32(c + 12) == 0);
      CHECK("width/height/stride", fwcfg_get_be32(c + 16) == 1024 && fwcfg_get_be32(c + 20) == 768 && fwcfg_get_be32(c + 24) == 4096);
      CHECK("high 64-bit address survives", ramfb_build_cfg(c, 0x1122334455667788ULL, RAMFB_FOURCC_XR24, 16, 16, 64) == FWCFG_OK && fwcfg_get_be32(c) == 0x11223344u && fwcfg_get_be32(c + 4) == 0x55667788u);
      CHECK("stride below width*4 refused", ramfb_build_cfg(c, 0x4FD00000ULL, RAMFB_FOURCC_XR24, 1024, 768, 4092) == FWCFG_EARG);
      CHECK("width below QEMU's minimum refused", ramfb_build_cfg(c, 0x4FD00000ULL, RAMFB_FOURCC_XR24, 15, 768, 4096) == FWCFG_EARG);
      CHECK("height below QEMU's minimum refused", ramfb_build_cfg(c, 0x4FD00000ULL, RAMFB_FOURCC_XR24, 1024, 0, 4096) == FWCFG_EARG);
      CHECK("unsupported fourcc refused", ramfb_build_cfg(c, 0x4FD00000ULL, 0x34324742u, 1024, 768, 4096) == FWCFG_EARG);
      CHECK("huge width with 32-bit stride does not wrap", ramfb_build_cfg(c, 0, RAMFB_FOURCC_XR24, 0x40000000u, 16, 0xFFFFFFFFu) == FWCFG_EARG);
      CHECK("null out refused", ramfb_build_cfg(0, 0, RAMFB_FOURCC_XR24, 1024, 768, 4096) == FWCFG_EARG); }

    printf("error strings\n");
    { int ok = 1; for (int rc = 0; rc >= -11; rc--) { const char *s = fwcfg_strerror(rc); if (!s || !*s || !strcmp(s, "unknown fw_cfg error")) ok = 0; }
      CHECK("every defined code has a message", ok); CHECK("unknown code handled", strcmp(fwcfg_strerror(-99), "unknown fw_cfg error") == 0); }

    printf("\nfwcfg_test: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
