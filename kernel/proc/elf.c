/*
 * kernel/proc/elf.c — validation of the supported ELF64/AArch64 subset (see kernel/include/elf.h). Pure function of its input: it allocates
 * nothing, touches no kernel state and never indexes outside [file, file+len). Every length and offset is checked with a subtraction
 * against a value already proven in range, never by adding two untrusted numbers, so no check can wrap.
 */
#include <elf.h>

#define EHDR_SIZE 64u
#define PHDR_SIZE 56u
#define ET_EXEC 2u
#define EM_AARCH64 183u
#define PT_NULL 0u
#define PT_LOAD 1u
#define PT_NOTE 4u
#define PT_GNU_STACK 0x6474e551u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

const char *elf_strerror(int rc)
{
    switch (rc) {
    case ELF_OK: return "ok"; case ELF_ETRUNC: return "truncated"; case ELF_EMAGIC: return "bad magic"; case ELF_ECLASS: return "not 64-bit";
    case ELF_EDATA: return "not little-endian"; case ELF_EVERSION: return "bad version"; case ELF_EOSABI: return "unsupported ABI";
    case ELF_ETYPE: return "not a static executable (ET_EXEC)"; case ELF_EMACHINE: return "not AArch64"; case ELF_EHEADER: return "bad header size or flags";
    case ELF_EPHNUM: return "bad program-header table"; case ELF_ESEGTYPE: return "unsupported segment type"; case ELF_ESEGBOUNDS: return "segment outside the file";
    case ELF_ESEGSIZE: return "bad segment size"; case ELF_EALIGN: return "bad segment alignment"; case ELF_ERANGE: return "segment outside the load window";
    case ELF_EOVERLAP: return "segments share a page"; case ELF_EWX: return "writable and executable segment"; case ELF_EFLAGS: return "bad segment flags";
    case ELF_EENTRY: return "bad entry point"; case ELF_ETOOBIG: return "image too large"; case ELF_ENOLOAD: return "no loadable segment";
    default: return "unknown error";
    }
}

int elf_validate(const uint8_t *file, size_t len, struct elf_image *img)
{
    if (!img) return ELF_ETRUNC;
    uint8_t *z = (uint8_t *)img; for (size_t i = 0; i < sizeof *img; i++) z[i] = 0;
    if (!file) return ELF_ETRUNC;
    if (len > ELF_MAX_FILE) return ELF_ETOOBIG;
    if (len < EHDR_SIZE) return ELF_ETRUNC;

    /* ---- ELF header ---- */
    if (!(file[0] == 0x7F && file[1] == 'E' && file[2] == 'L' && file[3] == 'F')) return ELF_EMAGIC;
    if (file[4] != 2) return ELF_ECLASS;
    if (file[5] != 1) return ELF_EDATA;
    if (file[6] != 1) return ELF_EVERSION;
    for (int i = 7; i < 16; i++) if (file[i] != 0) return ELF_EOSABI;
    if (rd16(file + 16) != ET_EXEC) return ELF_ETYPE;
    if (rd16(file + 18) != EM_AARCH64) return ELF_EMACHINE;
    if (rd32(file + 20) != 1) return ELF_EVERSION;
    uint64_t entry = rd64(file + 24), phoff = rd64(file + 32);
    uint32_t eflags = rd32(file + 48);
    uint16_t ehsize = rd16(file + 52), phentsize = rd16(file + 54), phnum = rd16(file + 56);
    if (ehsize != EHDR_SIZE || phentsize != PHDR_SIZE || eflags != 0) return ELF_EHEADER;
    if (phnum == 0 || phnum > ELF_MAX_PHNUM) return ELF_EPHNUM;
    if (phoff < EHDR_SIZE || (phoff & 7) || phoff > len || (uint64_t)phnum * PHDR_SIZE > len - phoff) return ELF_EPHNUM;   /* the table is inside the file; len is small so the product cannot overflow */

    /* ---- program headers ---- */
    struct elf_image out;
    for (size_t i = 0; i < sizeof out; i++) ((uint8_t *)&out)[i] = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = file + phoff + (uint64_t)i * PHDR_SIZE;
        uint32_t type = rd32(ph), flags = rd32(ph + 4);
        uint64_t off = rd64(ph + 8), va = rd64(ph + 16), filesz = rd64(ph + 32), memsz = rd64(ph + 40), align = rd64(ph + 48);
        if (type == PT_NULL || type == PT_NOTE) continue;
        if (type == PT_GNU_STACK) { if (flags & ELF_PF_X) return ELF_EFLAGS; continue; }
        if (type != PT_LOAD) return ELF_ESEGTYPE;
        if (out.nseg >= ELF_MAX_SEGS) return ELF_EPHNUM;

        if (flags != ELF_PF_R && flags != (ELF_PF_R | ELF_PF_X) && flags != (ELF_PF_R | ELF_PF_W)) {
            if ((flags & ELF_PF_W) && (flags & ELF_PF_X)) return ELF_EWX;
            return ELF_EFLAGS;
        }
        if (align != ELF_PAGE || ((va ^ off) & (ELF_PAGE - 1))) return ELF_EALIGN;
        if (memsz == 0 || memsz < filesz) return ELF_ESEGSIZE;
        if (off > len || filesz > len - off) return ELF_ESEGBOUNDS;
        if (va < ELF_VA_MIN || va >= ELF_VA_MAX) return ELF_ERANGE;
        if (memsz > ELF_VA_MAX - va) return ELF_ERANGE;                                  /* end <= window end: no wrap, no overflow */

        uint64_t first = va / ELF_PAGE, last = (va + memsz - 1) / ELF_PAGE;              /* safe: va + memsz <= ELF_VA_MAX */
        for (int j = 0; j < out.nseg; j++) {
            uint64_t f2 = out.seg[j].vaddr / ELF_PAGE, l2 = (out.seg[j].vaddr + out.seg[j].memsz - 1) / ELF_PAGE;
            if (first <= l2 && f2 <= last) return ELF_EOVERLAP;
        }
        uint64_t np = last - first + 1;
        if (np > ELF_MAX_PAGES || out.pages + np > ELF_MAX_PAGES) return ELF_ETOOBIG;

        struct elf_seg *s = &out.seg[out.nseg++];
        s->vaddr = va; s->offset = (uint32_t)off; s->filesz = (uint32_t)filesz; s->memsz = (uint32_t)memsz; s->flags = flags;
        out.pages += (uint32_t)np;
    }
    if (out.nseg == 0) return ELF_ENOLOAD;

    /* ---- entry point: aligned, inside the file-backed bytes of an executable segment ---- */
    int ok = 0;
    if (!(entry & 3))
        for (int j = 0; j < out.nseg; j++) {
            const struct elf_seg *s = &out.seg[j];
            if ((s->flags & ELF_PF_X) && s->filesz >= 4 && entry >= s->vaddr && entry - s->vaddr <= (uint64_t)s->filesz - 4) ok = 1;   /* room for the whole first instruction */
        }
    if (!ok) return ELF_EENTRY;
    out.entry = entry;
    *img = out;
    return ELF_OK;
}
