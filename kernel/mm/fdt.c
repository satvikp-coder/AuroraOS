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

/* ------------------------------------------------------------------------------------------------
 * Platform description: interrupt controller and architected timer.
 *
 * Runs after fdt_parse() has validated the blob, so offsets are known to be in bounds. Recognises
 * a GICv2 ("arm,cortex-a15-gic" / "arm,gic-400": reg = distributor, CPU interface) and the ARMv8
 * timer node (interrupts = four <type number flags> triples: secure phys, non-secure phys, virtual,
 * hypervisor). Anything else is reported, never guessed.
 * ---------------------------------------------------------------------------------------------- */
static int has_string(const uint8_t *v, uint32_t len, const char *want)
{   /* property value is a list of NUL-terminated strings */
    uint32_t i = 0;
    while (i < len) {
        const char *s = (const char *)(v + i);
        uint32_t n = 0;
        while (i + n < len && s[n]) n++;
        if (i + n >= len) return 0;                 /* unterminated last string */
        if (streq(s, want)) return 1;
        i += n + 1;
    }
    return 0;
}

int fdt_platform_parse(const void *fdt, struct fdt_platform *out)
{
    struct fdt_info scratch;
    int rc = fdt_parse(fdt, &scratch);
    if (!out) return FDT_ERR_NULL;
    out->gic_version = 0; out->gicd_base = out->gicd_size = out->gicc_base = out->gicc_size = 0;
    out->have_timer = 0; out->timer_intid_phys_ns = out->timer_intid_virt = 0;
    if (rc) return rc;

    const uint8_t *b = (const uint8_t *)fdt;
    uint32_t off_struct = be32(b + 8), off_str = be32(b + 12), size_str = be32(b + 32), size_struct = be32(b + 36);
    uint32_t root_ac = 2, root_sc = 1;
    int depth = 0;
    int is_timer = 0, gic_ver = 0;
    const uint8_t *reg = 0, *irqs = 0; uint32_t reg_len = 0, irqs_len = 0;
    uint32_t o = off_struct, end = off_struct + size_struct;

    while ((uint64_t)o + 4 <= end) {
        uint32_t tok = be32(b + o); o += 4;
        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)(b + o); uint32_t n = 0;
            while ((uint64_t)o + n < end && name[n]) n++;
            depth++;
            if (depth == 2) { is_timer = gic_ver = 0; reg = irqs = 0; reg_len = irqs_len = 0; }
            o += (n + 1 + 3) & ~3u;
        } else if (tok == FDT_END_NODE) {
            if (depth == 2) {
                if (gic_ver == 2 && reg) {
                    uint32_t tuple = 4 * (root_ac + root_sc);
                    if (root_ac < 1 || root_ac > 2 || root_sc < 1 || root_sc > 2 || reg_len < 2 * tuple) return FDT_ERR_CELLS;
                    if (out->gic_version) return FDT_ERR_STRUCT;   /* two interrupt controllers: ambiguous, rejected */
                    out->gic_version = 2;
                    out->gicd_base = cells(reg, root_ac);                   out->gicd_size = cells(reg + 4 * root_ac, root_sc);
                    out->gicc_base = cells(reg + tuple, root_ac);           out->gicc_size = cells(reg + tuple + 4 * root_ac, root_sc);
                } else if (gic_ver == 3 && !out->gic_version) {
                    out->gic_version = 3;                                   /* recognised, not supported by this kernel */
                }
                if (is_timer && irqs && irqs_len >= 4 * 12) {
                    uint32_t t1 = be32(irqs + 12), n1 = be32(irqs + 16), t2 = be32(irqs + 24), n2 = be32(irqs + 28);
                    if (t1 == 1 && n1 < 16 && t2 == 1 && n2 < 16) {         /* PPIs only */
                        out->timer_intid_phys_ns = 16 + n1; out->timer_intid_virt = 16 + n2; out->have_timer = 1;
                    }
                }
            }
            depth--;
        } else if (tok == FDT_PROP) {
            uint32_t len = be32(b + o), noff = be32(b + o + 4); o += 8;
            const char *pname = (const char *)(b + off_str + noff);
            const uint8_t *val = b + o;
            if (noff < size_str) {
                if (depth == 1 && streq(pname, "#address-cells") && len == 4) root_ac = be32(val);
                else if (depth == 1 && streq(pname, "#size-cells") && len == 4) root_sc = be32(val);
                else if (depth == 2 && streq(pname, "compatible")) {
                    if (has_string(val, len, "arm,cortex-a15-gic") || has_string(val, len, "arm,gic-400")) gic_ver = 2;
                    else if (has_string(val, len, "arm,gic-v3")) gic_ver = 3;
                    if (has_string(val, len, "arm,armv8-timer")) is_timer = 1;
                } else if (depth == 2 && streq(pname, "reg")) { reg = val; reg_len = len; }
                else if (depth == 2 && streq(pname, "interrupts")) { irqs = val; irqs_len = len; }
            }
            o += (len + 3) & ~3u;
        } else if (tok == FDT_END) {
            break;
        }
    }
    return FDT_OK;
}

/* ------------------------------------------------------------------------------------------------
 * virtio-mmio transports: every depth-2 node with compatible "virtio,mmio" (reg = base,size; interrupts = <type num flags>,
 * SPI type 0 => GIC interrupt ID 32 + num). Runs on a blob fdt_parse() accepted. Nothing is assumed about how many exist.
 * ---------------------------------------------------------------------------------------------- */
static int fdt_mmio_nodes(const void *fdt, const char *compat, struct fdt_virtio *out, int max)
{
    struct fdt_info scratch;
    int rc = fdt_parse(fdt, &scratch);
    if (rc) return rc;
    if (!out || max <= 0) return FDT_ERR_NULL;

    const uint8_t *b = (const uint8_t *)fdt;
    uint32_t off_struct = be32(b + 8), off_str = be32(b + 12), size_str = be32(b + 32), size_struct = be32(b + 36);
    uint32_t root_ac = 2, root_sc = 1;
    int depth = 0, n = 0, is_virtio = 0;
    const uint8_t *reg = 0, *irq = 0; uint32_t reg_len = 0, irq_len = 0;
    uint32_t o = off_struct, end = off_struct + size_struct;

    while ((uint64_t)o + 4 <= end) {
        uint32_t tok = be32(b + o); o += 4;
        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)(b + o); uint32_t len = 0;
            while ((uint64_t)o + len < end && name[len]) len++;
            depth++;
            if (depth == 2) { is_virtio = 0; reg = irq = 0; reg_len = irq_len = 0; }
            o += (len + 1 + 3) & ~3u;
        } else if (tok == FDT_END_NODE) {
            if (depth == 2 && is_virtio && reg && n < max) {
                uint32_t tuple = 4 * (root_ac + root_sc);
                if (root_ac < 1 || root_ac > 2 || root_sc < 1 || root_sc > 2 || reg_len < tuple) return FDT_ERR_CELLS;
                out[n].base = cells(reg, root_ac); out[n].size = cells(reg + 4 * root_ac, root_sc);
                out[n].intid = (irq && irq_len >= 12 && be32(irq) == 0) ? 32 + be32(irq + 4) : 0;
                if (out[n].size == 0 || out[n].base + out[n].size < out[n].base) return FDT_ERR_RESERVATION;
                n++;
            }
            depth--;
        } else if (tok == FDT_PROP) {
            uint32_t len = be32(b + o), noff = be32(b + o + 4); o += 8;
            const char *pname = (const char *)(b + off_str + noff);
            const uint8_t *val = b + o;
            if (noff < size_str) {
                if (depth == 1 && streq(pname, "#address-cells") && len == 4) root_ac = be32(val);
                else if (depth == 1 && streq(pname, "#size-cells") && len == 4) root_sc = be32(val);
                else if (depth == 2 && streq(pname, "compatible") && has_string(val, len, compat)) is_virtio = 1;
                else if (depth == 2 && streq(pname, "reg")) { reg = val; reg_len = len; }
                else if (depth == 2 && streq(pname, "interrupts")) { irq = val; irq_len = len; }
            }
            o += (len + 3) & ~3u;
        } else if (tok == FDT_END) {
            break;
        }
    }
    return n;
}

int fdt_virtio_mmio(const void *fdt, struct fdt_virtio *out, int max) { return fdt_mmio_nodes(fdt, "virtio,mmio", out, max); }

/* fw_cfg MMIO nodes ("qemu,fw-cfg-mmio"); only top-level nodes with a reg property are reported. */
int fdt_fwcfg_mmio(const void *fdt, struct fdt_virtio *out, int max) { return fdt_mmio_nodes(fdt, "qemu,fw-cfg-mmio", out, max); }

