/*
 * tests/kernel-host/fdt_pmm_test.c — host-side tests for kernel/mm/fdt.c and the reservation
 * logic of kernel/mm/pmm.c, using synthetic device trees built here (no dtc needed).
 *
 * Build/run: tests/kernel-host/run.sh      Emit sample blobs for QEMU: fdt_pmm_test --emit DIR
 * Only pmm_init, pmm_reserve, pmm_reserve_range, pmm_is_reserved and stats are used: pmm_alloc_page() would write to
 * "physical" addresses and is not called on the host.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mm.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; printf("  PASS: %s\n", name); } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

/* ---------------- tiny FDT builder ---------------- */
struct fb {
    _Alignas(8) uint8_t out[16384];
    uint8_t st[8192]; uint32_t sl;
    char str[2048]; uint32_t strl;
    uint8_t rsv[2048]; uint32_t rsvl;
    int terminate_rsv;
    uint32_t total;
};
static void p32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t)(v >> 32)); p32(p + 4, (uint32_t)v); }
static void fb_init(struct fb *b) { memset(b, 0, sizeof *b); b->terminate_rsv = 1; }
static void tok(struct fb *b, uint32_t t) { p32(b->st + b->sl, t); b->sl += 4; }
static void fb_begin(struct fb *b, const char *name) { tok(b, 1); size_t n = strlen(name) + 1; memcpy(b->st + b->sl, name, n); b->sl += (uint32_t)((n + 3) & ~3u); }
static void fb_end(struct fb *b) { tok(b, 2); }
static uint32_t stroff(struct fb *b, const char *n) { uint32_t o = b->strl; size_t l = strlen(n) + 1; memcpy(b->str + b->strl, n, l); b->strl += (uint32_t)l; return o; }
static void fb_prop(struct fb *b, const char *name, const void *d, uint32_t len)
{ tok(b, 3); p32(b->st + b->sl, len); b->sl += 4; p32(b->st + b->sl, stroff(b, name)); b->sl += 4; memcpy(b->st + b->sl, d, len); b->sl += (len + 3) & ~3u; }
static void fb_u32(struct fb *b, const char *name, uint32_t v) { uint8_t t[4]; p32(t, v); fb_prop(b, name, t, 4); }
static void fb_empty(struct fb *b, const char *name) { fb_prop(b, name, "", 0); }
/* reg built from cell counts */
static void fb_reg(struct fb *b, const char *name, int ac, int sc, const uint64_t *vals, int ntuples)
{
    uint8_t t[256]; uint32_t n = 0;
    for (int i = 0; i < ntuples; i++) {
        uint64_t a = vals[2 * i], s = vals[2 * i + 1];
        if (ac == 2) { p64(t + n, a); n += 8; } else { p32(t + n, (uint32_t)a); n += 4; }
        if (sc == 2) { p64(t + n, s); n += 8; } else { p32(t + n, (uint32_t)s); n += 4; }
    }
    fb_prop(b, name, t, n);
}
static void fb_rsv(struct fb *b, uint64_t a, uint64_t s) { p64(b->rsv + b->rsvl, a); p64(b->rsv + b->rsvl + 8, s); b->rsvl += 16; }
static uint32_t fb_finish(struct fb *b)
{
    tok(b, 9);
    uint32_t rsv_size = b->rsvl + (b->terminate_rsv ? 16 : 0);
    uint32_t off_rsv = 40, off_struct = off_rsv + rsv_size, off_str = off_struct + b->sl;
    b->total = off_str + b->strl;
    memset(b->out, 0, sizeof b->out);
    p32(b->out, 0xd00dfeed); p32(b->out + 4, b->total); p32(b->out + 8, off_struct); p32(b->out + 12, off_str);
    p32(b->out + 16, off_rsv); p32(b->out + 20, 17); p32(b->out + 24, 16); p32(b->out + 28, 0);
    p32(b->out + 32, b->strl); p32(b->out + 36, b->sl);
    memcpy(b->out + off_rsv, b->rsv, b->rsvl);
    memcpy(b->out + off_struct, b->st, b->sl);
    memcpy(b->out + off_str, b->str, b->strl);
    return b->total;
}
/* QEMU-like base tree: root(2/2) + memory@40000000. Leaves the root node OPEN so callers can add children. */
static void base_open(struct fb *b, int ac, int sc, uint64_t ram_base, uint64_t ram_size)
{
    fb_init(b); fb_begin(b, "");
    if (ac >= 0) fb_u32(b, "#address-cells", (uint32_t)ac);
    if (sc >= 0) fb_u32(b, "#size-cells", (uint32_t)sc);
    uint64_t r[2] = { ram_base, ram_size };
    fb_begin(b, "memory@40000000"); fb_prop(b, "device_type", "memory", 7);
    fb_reg(b, "reg", ac < 0 ? 2 : ac, sc < 0 ? 1 : sc, r, 1); fb_end(b);
}
static void base_close(struct fb *b) { fb_end(b); fb_finish(b); }
static int parse(struct fb *b, struct fdt_info *i) { return fdt_parse(b->out, i); }

/* reserved-memory helpers */
static void rm_open(struct fb *b, int ac, int sc)
{ fb_begin(b, "reserved-memory"); if (ac >= 0) fb_u32(b, "#address-cells", (uint32_t)ac); if (sc >= 0) fb_u32(b, "#size-cells", (uint32_t)sc); fb_empty(b, "ranges"); }
static void rm_child(struct fb *b, const char *name, int ac, int sc, const uint64_t *vals, int n, int nomap_before, int nomap_after)
{ fb_begin(b, name); if (nomap_before) fb_empty(b, "no-map"); fb_reg(b, "reg", ac, sc, vals, n); if (nomap_after) fb_empty(b, "no-map"); fb_end(b); }

static void tests(void)
{
    struct fb b; struct fdt_info i; int rc;
    uint64_t r1[2] = { 0x4a000000ULL, 0x100000ULL };

    printf("-- valid trees --\n");
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); base_close(&b);
    rc = parse(&b, &i);
    CHECK("QEMU-like tree parses", rc == FDT_OK && i.valid && i.ram_base == 0x40000000ULL && i.ram_size == 0x10000000ULL && i.nreserved == 0);
    CHECK("totalsize recorded", i.totalsize == b.total);
    base_open(&b, -1, -1, 0x80000000ULL, 0x4000000ULL); base_close(&b);
    rc = parse(&b, &i);
    CHECK("missing cell properties use the spec defaults (2 address, 1 size)", rc == FDT_OK && i.ram_base == 0x80000000ULL && i.ram_size == 0x4000000ULL);
    base_open(&b, 1, 1, 0x20000000ULL, 0x8000000ULL); base_close(&b);
    rc = parse(&b, &i);
    CHECK("1/1 cell layout parses", rc == FDT_OK && i.ram_base == 0x20000000ULL && i.ram_size == 0x8000000ULL);

    printf("-- memory node errors --\n");
    fb_init(&b); fb_begin(&b, ""); fb_u32(&b, "#address-cells", 2); fb_u32(&b, "#size-cells", 2); fb_end(&b); fb_finish(&b);
    CHECK("no memory node is an error", parse(&b, &i) == FDT_ERR_NO_MEMORY && !i.valid);
    base_open(&b, 2, 2, 0x40000000ULL, 0); base_close(&b);
    CHECK("zero-size memory is an error", parse(&b, &i) == FDT_ERR_NO_MEMORY);
    base_open(&b, 2, 2, ~0ULL - 5, 0x1000); base_close(&b);
    CHECK("memory region that overflows is an error", parse(&b, &i) == FDT_ERR_NO_MEMORY);
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL);
    { uint64_t r[2] = { 0x100000000ULL, 0x10000000ULL }; fb_begin(&b, "memory@100000000"); fb_reg(&b, "reg", 2, 2, r, 1); fb_end(&b); }
    base_close(&b);
    CHECK("two memory nodes are rejected, not ignored", parse(&b, &i) == FDT_ERR_MULTI_MEMORY);
    fb_init(&b); fb_begin(&b, ""); fb_u32(&b, "#address-cells", 2); fb_u32(&b, "#size-cells", 2);
    { uint64_t r[4] = { 0x40000000ULL, 0x1000000ULL, 0x80000000ULL, 0x1000000ULL }; fb_begin(&b, "memory"); fb_reg(&b, "reg", 2, 2, r, 2); fb_end(&b); }
    fb_end(&b); fb_finish(&b);
    CHECK("two reg tuples in one memory node are rejected", parse(&b, &i) == FDT_ERR_MULTI_MEMORY);
    fb_init(&b); fb_begin(&b, ""); fb_u32(&b, "#address-cells", 2); fb_u32(&b, "#size-cells", 2);
    { uint64_t r[2] = { 0x1000, 0x1000 }; fb_begin(&b, "memory-controller@1000"); fb_reg(&b, "reg", 2, 2, r, 1); fb_end(&b); }
    fb_end(&b); fb_finish(&b);
    CHECK("a node merely named memory-controller is not RAM", parse(&b, &i) == FDT_ERR_NO_MEMORY);
    base_open(&b, 3, 2, 0x40000000ULL, 0x1000); base_close(&b);
    CHECK("unsupported #address-cells is an error", parse(&b, &i) == FDT_ERR_CELLS);
    fb_init(&b); fb_begin(&b, ""); fb_u32(&b, "#address-cells", 2); fb_u32(&b, "#size-cells", 2);
    fb_begin(&b, "memory@0"); fb_prop(&b, "reg", "\0\0\0\0\0\0\0", 7); fb_end(&b); fb_end(&b); fb_finish(&b);
    CHECK("reg length not a multiple of the tuple size is an error", parse(&b, &i) == FDT_ERR_CELLS);

    printf("-- memory reserve map --\n");
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); fb_rsv(&b, 0x41000000ULL, 0x10000ULL); fb_rsv(&b, 0x42000000ULL, 0x2000ULL); base_close(&b);
    rc = parse(&b, &i);
    CHECK("reserve map entries are returned", rc == FDT_OK && i.nreserved == 2 && i.reserved[0].addr == 0x41000000ULL && i.reserved[1].size == 0x2000ULL);
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); for (int k = 0; k < FDT_MAX_RESERVED; k++) fb_rsv(&b, 0x41000000ULL + k * 0x10000, 0x1000); base_close(&b);
    CHECK("exactly FDT_MAX_RESERVED reservations are accepted", parse(&b, &i) == FDT_OK && i.nreserved == FDT_MAX_RESERVED);
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); for (int k = 0; k <= FDT_MAX_RESERVED; k++) fb_rsv(&b, 0x41000000ULL + k * 0x10000, 0x1000); base_close(&b);
    CHECK("one more than the capacity is REJECTED, not truncated", parse(&b, &i) == FDT_ERR_TOO_MANY_RESERVATIONS && !i.valid);
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); fb_rsv(&b, ~0ULL - 5, 100); base_close(&b);
    CHECK("reserve-map address+size overflow is an error", parse(&b, &i) == FDT_ERR_RESERVATION);
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); fb_rsv(&b, 0x41000000ULL, 0); base_close(&b);
    CHECK("zero-size non-terminator reserve entry is an error", parse(&b, &i) == FDT_ERR_RESERVATION);
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); b.terminate_rsv = 0; fb_rsv(&b, 0x41000000ULL, 0x1000); base_close(&b);
    CHECK("unterminated reserve map is an error", parse(&b, &i) < 0);
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); fb_rsv(&b, 0x41000000ULL, 0x4000); fb_rsv(&b, 0x41002000ULL, 0x4000); base_close(&b);
    CHECK("overlapping reservations are both reported (allocator merges them)", parse(&b, &i) == FDT_OK && i.nreserved == 2);

    printf("-- /reserved-memory --\n");
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); rm_open(&b, 2, 2); rm_child(&b, "fw@4a000000", 2, 2, r1, 1, 0, 0); fb_end(&b); base_close(&b);
    rc = parse(&b, &i);
    CHECK("reserved-memory child with static reg is reported", rc == FDT_OK && i.nreserved == 1 && i.reserved[0].addr == 0x4a000000ULL && i.reserved[0].size == 0x100000ULL && !i.reserved[0].nomap);
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); rm_open(&b, 2, 2); rm_child(&b, "a", 2, 2, r1, 1, 1, 0); rm_child(&b, "b", 2, 2, r1, 1, 0, 1); fb_end(&b); base_close(&b);
    rc = parse(&b, &i);
    CHECK("no-map is recorded whether it comes before or after reg", rc == FDT_OK && i.nreserved == 2 && i.reserved[0].nomap && i.reserved[1].nomap);
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); rm_open(&b, 2, 2); rm_child(&b, "a", 2, 2, r1, 1, 0, 0); rm_child(&b, "b", 2, 2, r1, 1, 1, 0); fb_end(&b); base_close(&b);
    parse(&b, &i);
    CHECK("no-map does not leak to sibling nodes", i.nreserved == 2 && !i.reserved[0].nomap && i.reserved[1].nomap);
    { uint64_t two[4] = { 0x4a000000ULL, 0x1000, 0x4b000000ULL, 0x2000 };
      base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); rm_open(&b, 2, 2); rm_child(&b, "m", 2, 2, two, 2, 0, 0); fb_end(&b); base_close(&b);
      rc = parse(&b, &i); CHECK("multiple reg tuples in one child are all reserved", rc == FDT_OK && i.nreserved == 2 && i.reserved[1].addr == 0x4b000000ULL); }
    { uint64_t c11[2] = { 0x4c000000ULL, 0x4000 };
      base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); rm_open(&b, 1, 1); rm_child(&b, "x", 1, 1, c11, 1, 0, 0); fb_end(&b); base_close(&b);
      rc = parse(&b, &i); CHECK("reserved-memory uses its OWN #address-cells/#size-cells", rc == FDT_OK && i.nreserved == 1 && i.reserved[0].addr == 0x4c000000ULL && i.reserved[0].size == 0x4000); }
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); rm_open(&b, 2, 2); fb_begin(&b, "pool"); fb_u32(&b, "size", 0x100000); fb_empty(&b, "reusable"); fb_end(&b); fb_end(&b); base_close(&b);
    CHECK("dynamic pools (size only, no reg) have nothing to exclude and are accepted", parse(&b, &i) == FDT_OK && i.nreserved == 0);
    { uint64_t bad[2] = { ~0ULL - 5, 0x1000 };
      base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); rm_open(&b, 2, 2); rm_child(&b, "o", 2, 2, bad, 1, 0, 0); fb_end(&b); base_close(&b);
      CHECK("reserved-memory overflow is an error", parse(&b, &i) == FDT_ERR_RESERVATION);
      uint64_t zero[2] = { 0x4a000000ULL, 0 };
      base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); rm_open(&b, 2, 2); rm_child(&b, "z", 2, 2, zero, 1, 0, 0); fb_end(&b); base_close(&b);
      CHECK("reserved-memory zero size is an error", parse(&b, &i) == FDT_ERR_RESERVATION); }
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); rm_open(&b, 2, 2); fb_begin(&b, "odd"); fb_prop(&b, "reg", "\0\0\0\0\0\0\0\0\0\0", 10); fb_end(&b); fb_end(&b); base_close(&b);
    CHECK("reserved-memory reg of the wrong length is an error", parse(&b, &i) == FDT_ERR_CELLS);
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); rm_open(&b, 2, 2);
    for (int k = 0; k <= FDT_MAX_RESERVED; k++) { char n[16]; snprintf(n, sizeof n, "r%d", k); uint64_t v[2] = { 0x41000000ULL + k * 0x10000, 0x1000 }; rm_child(&b, n, 2, 2, v, 1, 0, 0); }
    fb_end(&b); base_close(&b);
    CHECK("too many reserved-memory children is REJECTED, not truncated", parse(&b, &i) == FDT_ERR_TOO_MANY_RESERVATIONS);
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL);
    for (int k = 0; k < 20; k++) fb_rsv(&b, 0x41000000ULL + k * 0x10000, 0x1000);
    rm_open(&b, 2, 2);
    for (int k = 0; k < 20; k++) { char n[16]; snprintf(n, sizeof n, "r%d", k); uint64_t v[2] = { 0x45000000ULL + k * 0x10000, 0x1000 }; rm_child(&b, n, 2, 2, v, 1, 0, 0); }
    fb_end(&b); base_close(&b);
    CHECK("reserve map and reserved-memory share ONE capacity limit", parse(&b, &i) == FDT_ERR_TOO_MANY_RESERVATIONS);

    printf("-- malformed blobs --\n");
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); base_close(&b);
    CHECK("NULL blob / NULL out", fdt_parse(0, &i) == FDT_ERR_NULL && fdt_parse(b.out, 0) == FDT_ERR_NULL);
    CHECK("unaligned pointer", fdt_parse(b.out + 4, &i) == FDT_ERR_MAGIC);
    { struct fb c = b; p32(c.out, 0xdeadbeef); CHECK("bad magic", parse(&c, &i) == FDT_ERR_MAGIC); }
    { struct fb c = b; p32(c.out + 4, 20); CHECK("totalsize smaller than a header", parse(&c, &i) == FDT_ERR_HEADER); }
    { struct fb c = b; p32(c.out + 4, 0x7fffffff); CHECK("absurd totalsize", parse(&c, &i) == FDT_ERR_HEADER); }
    { struct fb c = b; p32(c.out + 8, b.total + 100); CHECK("structure offset beyond the blob", parse(&c, &i) == FDT_ERR_HEADER); }
    { struct fb c = b; p32(c.out + 36, 0x7ffffff0); CHECK("structure size beyond the blob", parse(&c, &i) == FDT_ERR_HEADER); }
    { struct fb c = b; p32(c.out + 12, b.total + 4); CHECK("strings offset beyond the blob", parse(&c, &i) == FDT_ERR_HEADER); }
    { struct fb c = b; p32(c.out + 8, 41); CHECK("unaligned structure offset", parse(&c, &i) == FDT_ERR_HEADER); }
    { struct fb c = b; p32(c.out + 20, 15); CHECK("version below 16", parse(&c, &i) == FDT_ERR_HEADER); }
    { struct fb c = b; p32(c.out + 16, b.total + 8); CHECK("reserve map offset beyond the blob", parse(&c, &i) == FDT_ERR_HEADER); }
    { struct fb c = b; uint32_t so = (uint32_t)((c.out[8] << 24) | (c.out[9] << 16) | (c.out[10] << 8) | c.out[11]);
      p32(c.out + so + 4, 0x7); /* turn the root BEGIN_NODE's following bytes into garbage token stream */
      memset(c.out + so, 0xff, 4); CHECK("unknown structure token", parse(&c, &i) == FDT_ERR_STRUCT); }
    { struct fb c = b; p32(c.out + 36, 8); CHECK("structure block truncated before FDT_END", parse(&c, &i) == FDT_ERR_STRUCT); }
    fb_init(&b); fb_begin(&b, ""); fb_u32(&b, "#address-cells", 2); fb_finish(&b);
    CHECK("missing FDT_END_NODE (unbalanced) is an error", parse(&b, &i) == FDT_ERR_STRUCT);
    fb_init(&b); for (int k = 0; k < 20; k++) fb_begin(&b, "n"); for (int k = 0; k < 20; k++) fb_end(&b); fb_finish(&b);
    CHECK("excessive nesting is an error", parse(&b, &i) == FDT_ERR_STRUCT);
    fb_init(&b); fb_begin(&b, ""); fb_end(&b); fb_end(&b); fb_finish(&b);
    CHECK("END_NODE without BEGIN_NODE is an error", parse(&b, &i) == FDT_ERR_STRUCT);

    printf("-- strings block validation --\n");
    base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL); base_close(&b);
    { struct fb c = b; uint32_t so = (c.out[8] << 24) | (c.out[9] << 16) | (c.out[10] << 8) | c.out[11];
      /* first property of the root is '#address-cells' (token at so+8: BEGIN_NODE + 4 bytes name pad, then PROP tok,len,nameoff) */
      uint32_t pnoff = so + 4 + 4 + 4 + 4;  /* BEGIN_NODE, "" padded, PROP token, len -> nameoff */
      p32(c.out + pnoff, 0x100000); CHECK("property name offset beyond the strings block", parse(&c, &i) == FDT_ERR_STRING); }
    { struct fb c = b; uint32_t strsz = b.strl; p32(c.out + 32, 3); /* strings block shrunk so names are cut off before their NUL */
      (void)strsz; CHECK("property name not NUL-terminated inside the strings block", parse(&c, &i) == FDT_ERR_STRING); }
    { struct fb c = b; uint32_t off_str = (c.out[12] << 24) | (c.out[13] << 16) | (c.out[14] << 8) | c.out[15];
      memset(c.out + off_str, 'A', b.strl); /* no NUL anywhere in the strings block */
      CHECK("strings block without any NUL terminator", parse(&c, &i) == FDT_ERR_STRING); }
}

static void pmm_tests(void)
{
    struct pmm_stats s;
    printf("-- pmm reservation logic (host) --\n");
    CHECK("pmm_init rejects unaligned base and zero size", pmm_init(0x40000800UL, 0x10000000UL) < 0 && pmm_init(0x40000000UL, 0) < 0);
    CHECK("pmm_init", pmm_init(0x40000000UL, 0x10000000UL) == 0);
    pmm_get_stats(&s);
    CHECK("all pages initially free", s.total_pages == 65536 && s.free_pages == 65536 && s.reserved_pages == 0);
    CHECK("reserve rounds outward to whole pages", pmm_reserve(0x40000800UL, 0x40001800UL) == 0 && (pmm_get_stats(&s), s.reserved_pages == 2) && pmm_is_reserved(0x40000000UL) && pmm_is_reserved(0x40001000UL) && !pmm_is_reserved(0x40002000UL));
    CHECK("overlapping reservation is idempotent", pmm_reserve(0x40001000UL, 0x40003000UL) == 0 && (pmm_get_stats(&s), s.reserved_pages == 3));
    CHECK("partially below RAM is clamped", pmm_reserve(0x3FFFF000UL, 0x40004000UL) == 0 && (pmm_get_stats(&s), s.reserved_pages == 4));
    CHECK("partially above RAM is clamped", pmm_reserve(0x4FFFE000UL, 0x60000000UL) == 0 && pmm_is_reserved(0x4FFFF000UL) && pmm_is_reserved(0x4FFFE000UL) && (pmm_get_stats(&s), s.reserved_pages == 6));
    CHECK("entirely outside RAM is ignored", pmm_reserve(0x1000, 0x2000) == 0 && pmm_reserve(0x50000000UL, 0x50001000UL) == 0 && (pmm_get_stats(&s), s.reserved_pages == 6));
    CHECK("a region ending exactly at RAM base is outside", pmm_reserve(0x3FFFF000UL, 0x40000000UL) == 0 && (pmm_get_stats(&s), s.reserved_pages == 6));
    CHECK("end <= start is rejected", pmm_reserve(0x40005000UL, 0x40005000UL) == -1 && pmm_reserve(0x40006000UL, 0x40005000UL) == -1);
    CHECK("range overflow is rejected", pmm_reserve_range(~0ULL - 4095, 8192) == -3 && pmm_reserve_range(0x40000000UL, 0) == -3);
    CHECK("huge range reaching the end of the address space is clamped, not wrapped", pmm_reserve_range(0x4FF00000UL, ~0ULL - 0x4FF00000UL) == 0 && pmm_is_reserved(0x4FFFF000UL));
    CHECK("reserved pages cannot be freed", pmm_free_page(0x40000000UL) == PMM_ERR_RESERVED && pmm_free_page(0x4FFFF000UL) == PMM_ERR_RESERVED);
    CHECK("free of an unreserved, never-allocated page is a double free", pmm_free_page(0x40100000UL) == PMM_ERR_DOUBLE);
    pmm_get_stats(&s);
    CHECK("rejected frees are counted", s.free_errors == 3);
    CHECK("accounting is consistent", s.free_pages + s.reserved_pages + s.allocated_pages == s.total_pages);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--emit")) {
        /* Sample blobs for QEMU: -dtb <dir>/reserved.dtb (QEMU fills in the real /memory node). */
        struct fb b; char path[512]; uint64_t r1[2] = { 0x4a000000ULL, 0x100000ULL }, r2[2] = { 0x4b000000ULL, 0x4000ULL };
        base_open(&b, 2, 2, 0x40000000ULL, 0x10000000ULL);
        fb_rsv(&b, 0x4c000000ULL, 0x10000ULL);
        rm_open(&b, 2, 2); rm_child(&b, "fw@4a000000", 2, 2, r1, 1, 0, 0); rm_child(&b, "hole@4b000000", 2, 2, r2, 1, 1, 0); fb_end(&b);
        base_close(&b);
        snprintf(path, sizeof path, "%s/reserved.dtb", argv[2]);
        FILE *f = fopen(path, "wb"); if (!f) { perror("fopen"); return 2; }
        fwrite(b.out, 1, b.total, f); fclose(f); printf("wrote %s (%u bytes)\n", path, b.total);
        return 0;
    }
    tests();
    pmm_tests();
    printf("\n== Summary ==\nPassed: %d\nFailed: %d\n", passed, failed);
    return failed ? 1 : 0;
}
