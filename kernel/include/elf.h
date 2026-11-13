#ifndef AURORA_ELF_H
#define AURORA_ELF_H
#include <stddef.h>
#include <stdint.h>

/*
 * AuroraOS executable format: a deliberately NARROW subset of ELF64 for AArch64. Pure validation (kernel/proc/elf.c): no allocation, no
 * kernel state, no trust in any byte. See docs/kernel/loader.md for the full contract.
 *
 * Accepted: a little-endian, 64-bit, statically linked ET_EXEC for EM_AARCH64, e_flags 0, with 1..ELF_MAX_PHNUM program headers of which
 *   - PT_LOAD   (1..ELF_MAX_SEGS of them): flags exactly R, R+X or R+W (never W+X), p_align 4096, vaddr and file offset congruent mod 4096,
 *                memsz >= filesz > or = 0 (memsz > 0), wholly inside the user load window, no two segments sharing a page;
 *   - PT_NULL, PT_NOTE: ignored;  PT_GNU_STACK: accepted only without the execute flag (the stack is never executable here anyway).
 * Everything else is refused with a specific error: PT_INTERP, PT_DYNAMIC, PT_TLS, PT_SHLIB, unknown types, ET_DYN / PIE (that would need
 * relocation), executable stacks, section headers are NOT consulted at all (they are not needed to run a static image and are not trusted).
 * The entry point must be 4-byte aligned and inside the FILE-BACKED part of an executable segment.
 */

/* The load window: a loaded image lives in [ELF_VA_MIN, ELF_VA_MAX). Below it are the fixed low user pages (code, data, guard, stack) that every
 * process layout reserves; the stack and its guard page therefore can never be overlapped by an image segment. */
#define ELF_VA_MIN       0x4000010000ULL         /* USER_VA_BASE + 64 KiB */
#define ELF_VA_MAX       0x4000100000ULL         /* USER_VA_BASE + 1 MiB */
#define ELF_PAGE         4096ULL
#define ELF_MAX_PHNUM    8
#define ELF_MAX_SEGS     4
#define ELF_MAX_PAGES    12                      /* total image pages (all segments, bss included) */
#define ELF_MAX_FILE     (48u * 512u)            /* the largest file AuroraFS can hold */

#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define ELF_PF_R 4u

enum {
    ELF_OK = 0,
    ELF_ETRUNC = -1,      /* shorter than the ELF header, or the program-header table runs past the end */
    ELF_EMAGIC = -2,
    ELF_ECLASS = -3,      /* not 64-bit */
    ELF_EDATA = -4,       /* not little-endian */
    ELF_EVERSION = -5,
    ELF_EOSABI = -6,      /* OSABI / ABI version / padding not zero */
    ELF_ETYPE = -7,       /* not ET_EXEC (this includes ET_DYN, i.e. position-independent images that need relocation) */
    ELF_EMACHINE = -8,    /* not AArch64 */
    ELF_EHEADER = -9,     /* e_ehsize / e_phentsize / e_flags wrong */
    ELF_EPHNUM = -10,     /* zero or too many program headers, table misplaced or misaligned, too many PT_LOAD */
    ELF_ESEGTYPE = -11,   /* a program-header type this loader does not support (INTERP, DYNAMIC, TLS, ...) */
    ELF_ESEGBOUNDS = -12, /* segment file range outside the file, or arithmetic overflow */
    ELF_ESEGSIZE = -13,   /* memsz < filesz, memsz == 0, or one segment larger than the window */
    ELF_EALIGN = -14,     /* p_align is not 4096, or vaddr and offset are not congruent modulo 4096 */
    ELF_ERANGE = -15,     /* outside the load window (includes the low pages with the stack and guard page) or wraps */
    ELF_EOVERLAP = -16,   /* two segments share a page */
    ELF_EWX = -17,        /* writable and executable */
    ELF_EFLAGS = -18,     /* a segment without PF_R, with unknown flag bits, or PT_GNU_STACK asking for execute */
    ELF_EENTRY = -19,     /* entry point unaligned or not inside the file-backed part of an executable segment */
    ELF_ETOOBIG = -20,    /* file larger than ELF_MAX_FILE or more than ELF_MAX_PAGES pages in total */
    ELF_ENOLOAD = -21     /* no PT_LOAD segment */
};

struct elf_seg {
    uint64_t vaddr;       /* as in the file (not necessarily page aligned; congruent to offset mod 4096) */
    uint32_t offset;      /* file offset of the initialised bytes */
    uint32_t filesz;      /* bytes copied from the file */
    uint32_t memsz;       /* bytes mapped; memsz - filesz bytes are zero (bss) */
    uint32_t flags;       /* ELF_PF_R, ELF_PF_R|ELF_PF_X or ELF_PF_R|ELF_PF_W */
};

struct elf_image {
    uint64_t entry;
    int nseg;
    struct elf_seg seg[ELF_MAX_SEGS];
    uint32_t pages;       /* total pages the image needs */
};

/* Validate `file` (untrusted) and describe it. On any error `img` is zeroed and nothing else has happened. */
int elf_validate(const uint8_t *file, size_t len, struct elf_image *img);
const char *elf_strerror(int rc);

#endif
