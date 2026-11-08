/*
 * kernel/mm/fdt.c — minimal, bounds-checked flattened-device-tree reader.
 *
 * Extracts exactly what the memory manager needs, per the Devicetree Specification v0.4:
 *   - the single /memory node            (chapter 3.4)
 *   - the memory reserve map             (section 5.3)
 *   - /reserved-memory child nodes       (section 3.5.4) that carry a static `reg`
 *
 * Fail-closed rules: anything malformed, ambiguous or not representable is an ERROR
 * (never silently ignored or truncated): bad offsets, unterminated names, address/size
 * overflow, zero-size reservations, more reservations than FDT_MAX_RESERVED, more than
 * one RAM region, unsupported #address-cells/#size-cells.
 *
 * Deliberately not handled (and why that is safe): /reserved-memory children that give
 * only `size` (dynamic allocation) have no fixed address, and this kernel never uses
 * such pools, so there is nothing to exclude. `no-map` is recorded so the MMU code can
 * leave the region unmapped.
 */
#include <mm.h>

#define FDT_MAGIC 0xd00dfeedU
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9
#define FDT_MAX_DEPTH 16

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int starts(const char *s, const char *pre) { while (*pre) { if (*s++ != *pre++) return 0; } return 1; }
static uint64_t cells(const uint8_t *p, uint32_t n) { uint64_t v = 0; for (uint32_t i = 0; i < n; i++) v = (v << 32) | be32(p + 4 * i); return v; }

const char *fdt_strerror(int e)
{
    switch (e) {
    case FDT_OK: return "ok";
    case FDT_ERR_NULL: return "null pointer";
    case FDT_ERR_MAGIC: return "bad magic or alignment";
    case FDT_ERR_HEADER: return "inconsistent header";
    case FDT_ERR_STRUCT: return "malformed structure block";
    case FDT_ERR_CELLS: return "unsupported or malformed cell sizes";
    case FDT_ERR_NO_MEMORY: return "no usable memory node";
    case FDT_ERR_TOO_MANY_RESERVATIONS: return "too many reserved regions";
    case FDT_ERR_MULTI_MEMORY: return "multiple RAM regions unsupported";
    case FDT_ERR_RESERVATION: return "malformed reservation (zero size or overflow)";
    case FDT_ERR_STRING: return "unterminated property name";
    default: return "unknown error";
    }
}

static int add_reservation(struct fdt_info *o, uint64_t addr, uint64_t size, int nomap)
{
    if (size == 0 || addr + size < addr) return FDT_ERR_RESERVATION;
    if (o->nreserved >= FDT_MAX_RESERVED) return FDT_ERR_TOO_MANY_RESERVATIONS;
    o->reserved[o->nreserved].addr = addr;
    o->reserved[o->nreserved].size = size;
    o->reserved[o->nreserved].nomap = nomap;
    o->nreserved++;
    return FDT_OK;
}

/* Node kinds we care about, tracked per depth. Root is depth 1, its children depth 2. */
enum { K_OTHER = 0, K_MEMORY, K_RESERVED_PARENT, K_RESERVED_CHILD };

int fdt_parse(const void *fdt, struct fdt_info *out)
{
    const uint8_t *b = (const uint8_t *)fdt;
    if (!out) return FDT_ERR_NULL;
    out->valid = 0; out->nreserved = 0; out->ram_base = out->ram_size = 0; out->totalsize = 0;
    if (!b) return FDT_ERR_NULL;
    if (((uintptr_t)b & 7) || be32(b) != FDT_MAGIC) return FDT_ERR_MAGIC;

    uint32_t total = be32(b + 4), off_struct = be32(b + 8), off_str = be32(b + 12), off_rsv = be32(b + 16);
    uint32_t version = be32(b + 20), last_comp = be32(b + 24);
    uint32_t size_str = be32(b + 32), size_struct = be32(b + 36);
    if (total < 40 || total > (64u << 20)) return FDT_ERR_HEADER;
    if (version < 16 || last_comp > 17 || last_comp > version) return FDT_ERR_HEADER;
    if ((off_struct & 3) || (size_struct & 3) || (off_rsv & 7)) return FDT_ERR_HEADER;
    if ((uint64_t)off_struct + size_struct > total || (uint64_t)off_str + size_str > total || off_rsv < 40 || off_rsv > total) return FDT_ERR_HEADER;
    out->totalsize = total;

    /* Memory reserve map: (addr,size) pairs, terminated by an all-zero entry that must be inside the blob. */
    int terminated = 0;
    for (uint32_t o = off_rsv; (uint64_t)o + 16 <= total; o += 16) {
        uint64_t a = be64(b + o), s = be64(b + o + 8);
        if (a == 0 && s == 0) { terminated = 1; break; }
        int r = add_reservation(out, a, s, 0);
        if (r) return r;
    }
    if (!terminated) return FDT_ERR_HEADER;

    uint32_t root_ac = 2, root_sc = 1;        /* defaults per the spec when the properties are absent */
    uint32_t rm_ac = 2, rm_sc = 1;            /* #address-cells/#size-cells of /reserved-memory */
    int kind[FDT_MAX_DEPTH + 1] = { 0 };
    int depth = 0, banks = 0, ended = 0;
    int node_nomap = 0, node_res_start = 0;
    uint32_t o = off_struct, end = off_struct + size_struct;

    while ((uint64_t)o + 4 <= end) {
        uint32_t tok = be32(b + o); o += 4;
        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)(b + o);
            uint32_t n = 0;
            while ((uint64_t)o + n < end && name[n]) n++;
            if ((uint64_t)o + n >= end) return FDT_ERR_STRUCT;      /* name not terminated inside the block */
            if (++depth > FDT_MAX_DEPTH) return FDT_ERR_STRUCT;
            kind[depth] = K_OTHER;
            if (depth == 2 && (streq(name, "memory") || starts(name, "memory@"))) kind[depth] = K_MEMORY;
            else if (depth == 2 && streq(name, "reserved-memory")) kind[depth] = K_RESERVED_PARENT;
            else if (depth == 3 && kind[2] == K_RESERVED_PARENT) { kind[depth] = K_RESERVED_CHILD; node_nomap = 0; node_res_start = out->nreserved; }
            o += (n + 1 + 3) & ~3u;
        } else if (tok == FDT_END_NODE) {
            if (depth <= 0) return FDT_ERR_STRUCT;
            kind[depth--] = K_OTHER;
        } else if (tok == FDT_PROP) {
            if (depth <= 0 || (uint64_t)o + 8 > end) return FDT_ERR_STRUCT;
            uint32_t len = be32(b + o), noff = be32(b + o + 4); o += 8;
            if ((uint64_t)o + len > end) return FDT_ERR_STRUCT;
            if (noff >= size_str) return FDT_ERR_STRING;
            /* The property name must be NUL-terminated before the end of the strings block. */
            uint32_t k = noff;
            while (k < size_str && b[off_str + k]) k++;
            if (k >= size_str) return FDT_ERR_STRING;
            const char *pname = (const char *)(b + off_str + noff);
            const uint8_t *val = b + o;

            if (depth == 1 && streq(pname, "#address-cells") && len == 4) root_ac = be32(val);
            else if (depth == 1 && streq(pname, "#size-cells") && len == 4) root_sc = be32(val);
            else if (kind[depth] == K_RESERVED_PARENT && streq(pname, "#address-cells") && len == 4) rm_ac = be32(val);
            else if (kind[depth] == K_RESERVED_PARENT && streq(pname, "#size-cells") && len == 4) rm_sc = be32(val);
            else if (kind[depth] == K_MEMORY && streq(pname, "reg")) {
                if (root_ac < 1 || root_ac > 2 || root_sc < 1 || root_sc > 2) return FDT_ERR_CELLS;
                uint32_t tuple = 4 * (root_ac + root_sc);
                if (len == 0 || len % tuple) return FDT_ERR_CELLS;
                for (uint32_t t = 0; t < len / tuple; t++) {
                    uint64_t a = cells(val + t * tuple, root_ac), s = cells(val + t * tuple + 4 * root_ac, root_sc);
                    if (s == 0 || a + s < a) return FDT_ERR_NO_MEMORY;
                    if (++banks > 1) return FDT_ERR_MULTI_MEMORY;
                    out->ram_base = a; out->ram_size = s;
                }
            } else if (kind[depth] == K_RESERVED_CHILD && streq(pname, "reg")) {
                if (rm_ac < 1 || rm_ac > 2 || rm_sc < 1 || rm_sc > 2) return FDT_ERR_CELLS;
                uint32_t tuple = 4 * (rm_ac + rm_sc);
                if (len == 0 || len % tuple) return FDT_ERR_CELLS;
                for (uint32_t t = 0; t < len / tuple; t++) {
                    int r = add_reservation(out, cells(val + t * tuple, rm_ac), cells(val + t * tuple + 4 * rm_ac, rm_sc), node_nomap);
                    if (r) return r;
                }
            } else if (kind[depth] == K_RESERVED_CHILD && streq(pname, "no-map")) {
                node_nomap = 1;
                for (int i = node_res_start; i < out->nreserved; i++) out->reserved[i].nomap = 1;
            }
            o += (len + 3) & ~3u;
        } else if (tok == FDT_NOP) {
            /* skip */
        } else if (tok == FDT_END) {
            ended = 1;
            break;
        } else {
            return FDT_ERR_STRUCT;
        }
    }
    if (!ended || depth != 0) return FDT_ERR_STRUCT;                 /* truncated or unbalanced */
    if (banks == 0 || out->ram_size == 0) return FDT_ERR_NO_MEMORY;
    out->valid = 1;
    return FDT_OK;
}
