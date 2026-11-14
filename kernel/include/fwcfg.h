/*
 * kernel/include/fwcfg.h - QEMU fw_cfg (MMIO flavour, as found on the ARM "virt" machine) and the ramfb configuration file.
 *
 * Sources (verified for G2, see docs/kernel/native-graphics-plan.md section 10):
 *   - https://www.qemu.org/docs/master/specs/fw_cfg.html   register layout, selector keys, file directory, DMA interface
 *   - QEMU hw/display/ramfb.c                              struct RAMFBCfg (28 bytes, packed, big-endian fields)
 *   - the installed QEMU 11.1.0, observed at run time
 *
 * Layout: the protocol helpers here are PURE (no hardware access) and are unit-tested on the host through a small
 * ops table; kernel/drivers/fwcfg.c supplies the MMIO implementation. Everything on the wire is BIG-endian except the
 * feature bitmap (key 0x0001), which is a LITTLE-endian u32. Nothing here assumes the CPU byte order.
 */
#ifndef AURORA_FWCFG_H
#define AURORA_FWCFG_H
#include <stddef.h>
#include <stdint.h>

/* ---- selector keys and feature bits ---------------------------------- */
#define FWCFG_KEY_SIGNATURE 0x0000u   /* four bytes "QEMU" */
#define FWCFG_KEY_ID        0x0001u   /* little-endian u32 feature bitmap */
#define FWCFG_KEY_FILE_DIR  0x0019u   /* file directory */
#define FWCFG_FEAT_TRADITIONAL (1u << 0)
#define FWCFG_FEAT_DMA         (1u << 1)

/* ---- MMIO register offsets (generic fw_cfg MMIO layout) --------------- */
#define FWCFG_MMIO_DATA 0x00u         /* data register (8/16/32/64-bit accesses, string-preserving) */
#define FWCFG_MMIO_SEL  0x08u         /* 16-bit selector, big-endian */
#define FWCFG_MMIO_DMA  0x10u         /* 64-bit DMA address register, big-endian; reads back "QEMU CFG" */
#define FWCFG_MMIO_SPAN 0x18u         /* size of the register block */
#define FWCFG_DMA_SIGNATURE 0x51454d5520434647ULL   /* "QEMU CFG" */

/* ---- directory ------------------------------------------------------- */
#define FWCFG_NAME_LEN   56u
#define FWCFG_ENTRY_LEN  64u          /* be32 size, be16 select, be16 reserved, char name[56] */
#define FWCFG_MAX_FILES  1024u        /* a larger directory is refused (the real one has a few dozen entries) */

/* ---- DMA descriptor (all fields big-endian; control is at the lowest address) ---- */
#define FWCFG_DMA_ERROR  (1u << 0)
#define FWCFG_DMA_READ   (1u << 1)
#define FWCFG_DMA_SKIP   (1u << 2)
#define FWCFG_DMA_SELECT (1u << 3)    /* the upper 16 bits of control then hold the selector */
#define FWCFG_DMA_WRITE  (1u << 4)
struct fwcfg_dma { uint8_t b[16]; } __attribute__((aligned(8)));   /* wire bytes: be32 control, be32 length, be64 address (no host byte order assumed) */

/* ---- result codes ---------------------------------------------------- */
enum {
    FWCFG_OK = 0,
    FWCFG_ENODEV = -1,    /* no fw_cfg node in the device tree */
    FWCFG_ENODE = -2,     /* node present but implausible (size, alignment, overflow, overlaps RAM) */
    FWCFG_ESIG = -3,      /* signature is not "QEMU" */
    FWCFG_ENODMA = -4,    /* DMA interface not offered (ramfb requires it) */
    FWCFG_EDIR = -5,      /* malformed file directory (count, entry, name, duplicate) */
    FWCFG_ENOFILE = -6,   /* named file not in the directory */
    FWCFG_EDMA = -7,      /* the device reported an error for a DMA request */
    FWCFG_ETIMEOUT = -8,  /* DMA did not complete within the bounded wait */
    FWCFG_EARG = -9,      /* bad argument (null, zero length, too large) */
    FWCFG_EMAP = -10,     /* the MMIO window could not be registered or mapped */
    FWCFG_ESTATE = -11,   /* driver used before a successful probe / map */
};
const char *fwcfg_strerror(int rc);

/* ---- pure protocol helpers (kernel/drivers/fwcfg_proto.c) --------------- */
uint16_t fwcfg_get_be16(const uint8_t *p);
uint32_t fwcfg_get_be32(const uint8_t *p);
void fwcfg_put_be32(uint8_t *p, uint32_t v);
void fwcfg_put_be64(uint8_t *p, uint64_t v);

struct fwcfg_ops {                    /* how the helpers reach a device (MMIO in the kernel, a fake in host tests) */
    void (*select)(void *ctx, uint16_t key);
    void (*read)(void *ctx, void *buf, uint32_t len);   /* sequential byte-stream read from the selected item */
};
struct fwcfg_file { uint32_t size; uint16_t select; };

/* Walk the directory and find EXACTLY `name`. FWCFG_EDIR if the directory is malformed (count over FWCFG_MAX_FILES, a name
 * with no NUL inside its 56 bytes, or the same name listed twice); FWCFG_ENOFILE if absent. Reads at most 4 + 64*count bytes. */
int fwcfg_find_file(const struct fwcfg_ops *ops, void *ctx, const char *name, struct fwcfg_file *out);
/* Signature "QEMU" check and feature bitmap; FWCFG_ESIG, or FWCFG_ENODMA when the DMA bit is absent and need_dma is set. */
int fwcfg_check_device(const struct fwcfg_ops *ops, void *ctx, int need_dma, uint32_t *features);

/* Fill a DMA descriptor (big-endian) for a write/read of `length` bytes at guest-physical `addr` on item `key`. */
void fwcfg_dma_fill(struct fwcfg_dma *d, uint16_t key, uint32_t ctl_op, uint32_t length, uint64_t addr);
/* Interpret a descriptor's control word AFTER the device was told to run it: 1 done, 0 still pending, FWCFG_EDMA on error. */
int fwcfg_dma_status(const volatile uint8_t *control_bytes);

/* ---- ramfb (etc/ramfb) -------------------------------------------------- */
#define RAMFB_FILE_NAME "etc/ramfb"
#define RAMFB_CFG_SIZE  28u           /* addr u64, fourcc u32, flags u32, width u32, height u32, stride u32 - all big-endian */
#define RAMFB_FOURCC_XR24 0x34325258u /* DRM_FORMAT_XRGB8888 = fourcc_code('X','R','2','4'); bytes B,G,R,X in memory */
#define RAMFB_MIN_DIM 16u             /* QEMU rejects anything smaller */
int ramfb_build_cfg(uint8_t out[RAMFB_CFG_SIZE], uint64_t addr, uint32_t fourcc, uint32_t width, uint32_t height, uint32_t stride);

/* ---- kernel driver (kernel/drivers/fwcfg.c) ----------------------------- */
/* probe is safe BEFORE the MMU is on (it only reads). fwcfg_map must follow vmm_init. */
int fwcfg_probe(const void *fdt, int need_dma);
int fwcfg_lookup(const char *name, struct fwcfg_file *out);        /* needs a successful probe */
int fwcfg_map(void);                                               /* register the window with the MMU policy and map it Device */
int fwcfg_dma_write(uint16_t key, const void *buf, uint32_t len);  /* DMA write to an item; buf must be physical RAM (identity-mapped) */
uint64_t fwcfg_base(void);

#endif
