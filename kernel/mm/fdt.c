/* kernel/mm/fdt.c — minimal, bounds-checked flattened-device-tree reader (memory node + reserve map). */
#include <mm.h>

#define FDT_MAGIC 0xd00dfeedU
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int starts(const char *s, const char *pre) { while (*pre) { if (*s++ != *pre++) return 0; } return 1; }

static uint64_t cells(const uint8_t *p, uint32_t n) { uint64_t v = 0; for (uint32_t i = 0; i < n; i++) v = (v << 32) | be32(p + 4 * i); return v; }

int fdt_parse(const void *fdt, struct fdt_info *out)
{
    const uint8_t *b = (const uint8_t *)fdt;
    if (!b || !out) return -1;
    out->valid = 0; out->nreserved = 0; out->ram_base = out->ram_size = 0; out->totalsize = 0;
    if (((uintptr_t)b & 3) || be32(b) != FDT_MAGIC) return -2;
    uint32_t total = be32(b + 4), off_struct = be32(b + 8), off_str = be32(b + 12), off_rsv = be32(b + 16);
    uint32_t size_str = be32(b + 32), size_struct = be32(b + 36);
    if (total < 40 || total > (64u << 20)) return -3;
    if ((uint64_t)off_struct + size_struct > total || (uint64_t)off_str + size_str > total || off_rsv > total) return -3;
    out->totalsize = total;

    /* Memory reserve map: (addr,size) pairs ending with (0,0). */
    for (uint32_t o = off_rsv; (uint64_t)o + 16 <= total; o += 16) {
        uint64_t a = be64(b + o), s = be64(b + o + 8);
        if (a == 0 && s == 0) break;
        if (out->nreserved < FDT_MAX_RESERVED) { out->reserved[out->nreserved].addr = a; out->reserved[out->nreserved].size = s; out->nreserved++; }
    }

    uint32_t addr_cells = 2, size_cells = 2;           /* defaults per the devicetree spec */
    int depth = 0, in_memory = 0, found = 0;
    uint32_t o = off_struct, end = off_struct + size_struct;
    while ((uint64_t)o + 4 <= end) {
        uint32_t tok = be32(b + o); o += 4;
        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)(b + o);
            uint32_t n = 0;
            while ((uint64_t)o + n < end && name[n]) n++;
            if ((uint64_t)o + n >= end) return -4;
            depth++;
            if (depth == 2 && starts(name, "memory")) in_memory = 1;
            o += (n + 1 + 3) & ~3u;
        } else if (tok == FDT_END_NODE) {
            if (depth == 2) in_memory = 0;
            if (--depth < 0) return -4;
        } else if (tok == FDT_PROP) {
            if ((uint64_t)o + 8 > end) return -4;
            uint32_t len = be32(b + o), noff = be32(b + o + 4); o += 8;
            if ((uint64_t)o + len > end || noff >= size_str) return -4;
            const char *pname = (const char *)(b + off_str + noff);
            const uint8_t *val = b + o;
            if (depth == 1 && streq(pname, "#address-cells") && len == 4) addr_cells = be32(val);
            else if (depth == 1 && streq(pname, "#size-cells") && len == 4) size_cells = be32(val);
            else if (in_memory && !found && streq(pname, "reg")) {
                if (addr_cells < 1 || addr_cells > 2 || size_cells < 1 || size_cells > 2) return -5;
                if (len < 4 * (addr_cells + size_cells)) return -5;
                out->ram_base = cells(val, addr_cells);
                out->ram_size = cells(val + 4 * addr_cells, size_cells);
                found = 1;
            }
            o += (len + 3) & ~3u;
        } else if (tok == FDT_NOP) {
            /* skip */
        } else if (tok == FDT_END) {
            break;
        } else {
            return -4;
        }
    }
    if (!found || out->ram_size == 0) return -6;
    out->valid = 1;
    return 0;
}
