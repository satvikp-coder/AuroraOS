/*
 * kernel/drivers/fwcfg_proto.c - pure fw_cfg / ramfb protocol helpers (no hardware access; unit-tested on the host).
 * See kernel/include/fwcfg.h for the wire formats and where each constant comes from.
 */
#include <fwcfg.h>

uint16_t fwcfg_get_be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
uint32_t fwcfg_get_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void fwcfg_put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
void fwcfg_put_be64(uint8_t *p, uint64_t v) { fwcfg_put_be32(p, (uint32_t)(v >> 32)); fwcfg_put_be32(p + 4, (uint32_t)v); }

const char *fwcfg_strerror(int rc)
{
    switch (rc) {
    case FWCFG_OK: return "ok";
    case FWCFG_ENODEV: return "no fw_cfg device in the device tree";
    case FWCFG_ENODE: return "fw_cfg device-tree node is implausible";
    case FWCFG_ESIG: return "fw_cfg signature is not QEMU";
    case FWCFG_ENODMA: return "fw_cfg has no DMA interface";
    case FWCFG_EDIR: return "malformed fw_cfg file directory";
    case FWCFG_ENOFILE: return "file not in the fw_cfg directory";
    case FWCFG_EDMA: return "fw_cfg DMA reported an error";
    case FWCFG_ETIMEOUT: return "fw_cfg DMA timed out";
    case FWCFG_EARG: return "bad argument";
    case FWCFG_EMAP: return "could not map the fw_cfg window";
    case FWCFG_ESTATE: return "fw_cfg used before probe/map";
    default: return "unknown fw_cfg error";
    }
}

/* 1 if the NUL-terminated `name` equals the entry name field exactly; 0 if different; -1 if the field has no NUL in 56 bytes. */
static int entry_name_matches(const uint8_t *field, const char *name)
{
    uint32_t i = 0;
    while (i < FWCFG_NAME_LEN && field[i]) i++;
    if (i == FWCFG_NAME_LEN) return -1;                 /* unterminated: malformed */
    uint32_t j = 0;
    while (j < i && name[j] && (uint8_t)name[j] == field[j]) j++;
    return (j == i && name[j] == 0) ? 1 : 0;
}

int fwcfg_find_file(const struct fwcfg_ops *ops, void *ctx, const char *name, struct fwcfg_file *out)
{
    if (!ops || !ops->select || !ops->read || !name || !name[0] || !out) return FWCFG_EARG;
    uint32_t nlen = 0;
    while (name[nlen]) { if (++nlen >= FWCFG_NAME_LEN) return FWCFG_EARG; }   /* a name that cannot fit in the field */

    uint8_t hdr[4];
    ops->select(ctx, FWCFG_KEY_FILE_DIR);
    ops->read(ctx, hdr, 4);
    uint32_t count = fwcfg_get_be32(hdr);
    if (count > FWCFG_MAX_FILES) return FWCFG_EDIR;

    int found = 0; struct fwcfg_file f = { 0, 0 };
    for (uint32_t i = 0; i < count; i++) {
        uint8_t e[FWCFG_ENTRY_LEN];
        ops->read(ctx, e, FWCFG_ENTRY_LEN);
        int m = entry_name_matches(e + 8, name);
        if (m < 0) return FWCFG_EDIR;                   /* any malformed name poisons the whole directory */
        if (m == 1) {
            if (found) return FWCFG_EDIR;               /* the same name twice is ambiguous: refuse */
            found = 1; f.size = fwcfg_get_be32(e); f.select = fwcfg_get_be16(e + 4);
            if (f.select < 0x20) return FWCFG_EDIR;     /* file items start at FW_CFG_FILE_FIRST (0x20) */
        }
    }
    if (!found) return FWCFG_ENOFILE;
    *out = f;
    return FWCFG_OK;
}

int fwcfg_check_device(const struct fwcfg_ops *ops, void *ctx, int need_dma, uint32_t *features)
{
    if (!ops || !ops->select || !ops->read) return FWCFG_EARG;
    uint8_t sig[4], id[4];
    ops->select(ctx, FWCFG_KEY_SIGNATURE);
    ops->read(ctx, sig, 4);
    if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U') return FWCFG_ESIG;
    ops->select(ctx, FWCFG_KEY_ID);
    ops->read(ctx, id, 4);
    uint32_t f = (uint32_t)id[0] | ((uint32_t)id[1] << 8) | ((uint32_t)id[2] << 16) | ((uint32_t)id[3] << 24);   /* little-endian */
    if (features) *features = f;
    if (need_dma && !(f & FWCFG_FEAT_DMA)) return FWCFG_ENODMA;
    return FWCFG_OK;
}

void fwcfg_dma_fill(struct fwcfg_dma *d, uint16_t key, uint32_t ctl_op, uint32_t length, uint64_t addr)
{
    fwcfg_put_be32(d->b, ctl_op | FWCFG_DMA_SELECT | ((uint32_t)key << 16));
    fwcfg_put_be32(d->b + 4, length);
    fwcfg_put_be64(d->b + 8, addr);
}

int fwcfg_dma_status(const volatile uint8_t *p)
{
    uint32_t c = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    if (c & FWCFG_DMA_ERROR) return FWCFG_EDMA;
    return c == 0 ? 1 : 0;
}

int ramfb_build_cfg(uint8_t out[RAMFB_CFG_SIZE], uint64_t addr, uint32_t fourcc, uint32_t width, uint32_t height, uint32_t stride)
{
    if (!out || width < RAMFB_MIN_DIM || height < RAMFB_MIN_DIM || fourcc != RAMFB_FOURCC_XR24) return FWCFG_EARG;
    if ((uint64_t)stride < (uint64_t)width * 4u) return FWCFG_EARG;        /* QEMU does not check this; we do */
    fwcfg_put_be64(out, addr);
    fwcfg_put_be32(out + 8, fourcc);
    fwcfg_put_be32(out + 12, 0);                                           /* flags: unused by QEMU */
    fwcfg_put_be32(out + 16, width);
    fwcfg_put_be32(out + 20, height);
    fwcfg_put_be32(out + 24, stride);
    return FWCFG_OK;
}
