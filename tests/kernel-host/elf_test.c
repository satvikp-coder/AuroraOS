/*
 * tests/kernel-host/elf_test.c — host tests for kernel/proc/elf.c (the AuroraOS ELF64/AArch64 subset validator). Built with ASan + UBSan.
 *
 * 1. Positive cases (the images it must accept, with the exact description it must produce).
 * 2. A table of ~60 negative cases, each a single deliberate defect applied to a valid image, each with the one error code it must produce.
 * 3. Deterministic mutation fuzzing: random byte flips, truncations and field overwrites. Every buffer is a malloc of EXACTLY its length, so
 *    ASan catches any read past the end; every accepted image is re-checked by an independent invariant checker.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, (uint16_t)v); w16(p + 2, (uint16_t)(v >> 16)); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }

struct ph { uint32_t type, flags; uint64_t off, va, filesz, memsz, align; };
#define PT_LOAD_ 1u
#define PF_RX (ELF_PF_R | ELF_PF_X)
#define PF_RW (ELF_PF_R | ELF_PF_W)

/* Build an image of `len` bytes (header + phdrs + deterministic filler). */
static uint8_t *build(size_t len, uint64_t entry, const struct ph *p, int n)
{
    uint8_t *b = calloc(1, len ? len : 1);
    if (len >= 64) {
        b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 2; b[5] = 1; b[6] = 1;
        w16(b + 16, 2); w16(b + 18, 183); w32(b + 20, 1); w64(b + 24, entry); w64(b + 32, 64); w32(b + 48, 0);
        w16(b + 52, 64); w16(b + 54, 56); w16(b + 56, (uint16_t)n);
    }
    for (int i = 0; i < n && 64 + (size_t)(i + 1) * 56 <= len; i++) {
        uint8_t *q = b + 64 + (size_t)i * 56;
        w32(q, p[i].type); w32(q + 4, p[i].flags); w64(q + 8, p[i].off); w64(q + 16, p[i].va); w64(q + 24, p[i].va);
        w64(q + 32, p[i].filesz); w64(q + 40, p[i].memsz); w64(q + 48, p[i].align);
    }
    for (size_t i = 64 + (size_t)n * 56; i < len; i++) b[i] = (uint8_t)(i * 31 + 7);
    return b;
}

#define TEXT_VA (ELF_VA_MIN + 0xB0)            /* offset 0xB0 follows the header and two program headers (64 + 112) */
#define DATA_VA (ELF_VA_MIN + 0x1000 + 0x300)
#define IMG_LEN 0x500
static struct ph base_ph[2];
static void init_base(void)
{
    base_ph[0] = (struct ph){ PT_LOAD_, PF_RX, 0xB0, TEXT_VA, 0x200, 0x200, ELF_PAGE };
    base_ph[1] = (struct ph){ PT_LOAD_, PF_RW, 0x300, DATA_VA, 0x100, 0x900, ELF_PAGE };
}

/* ---- independent invariant checker for anything the validator accepts ---- */
static int invariants(size_t len, const struct elf_image *im)
{
    if (im->nseg < 1 || im->nseg > ELF_MAX_SEGS || im->pages < 1 || im->pages > ELF_MAX_PAGES) return 0;
    uint32_t pages = 0; int entry_ok = 0;
    for (int i = 0; i < im->nseg; i++) {
        const struct elf_seg *s = &im->seg[i];
        if (s->flags != ELF_PF_R && s->flags != PF_RX && s->flags != PF_RW) return 0;               /* readable, never W+X */
        if (s->memsz == 0 || s->memsz < s->filesz) return 0;
        if ((size_t)s->offset + s->filesz > len) return 0;
        if (s->vaddr < ELF_VA_MIN || s->vaddr + s->memsz > ELF_VA_MAX) return 0;
        if ((s->vaddr & 0xFFF) != (s->offset & 0xFFF)) return 0;
        uint64_t f = s->vaddr >> 12, l = (s->vaddr + s->memsz - 1) >> 12; pages += (uint32_t)(l - f + 1);
        for (int j = 0; j < i; j++) { uint64_t f2 = im->seg[j].vaddr >> 12, l2 = (im->seg[j].vaddr + im->seg[j].memsz - 1) >> 12; if (f <= l2 && f2 <= l) return 0; }
        if ((s->flags & ELF_PF_X) && s->filesz >= 4 && im->entry >= s->vaddr && im->entry - s->vaddr <= (uint64_t)s->filesz - 4) entry_ok = 1;
    }
    return pages == im->pages && !(im->entry & 3) && entry_ok;
}

static int try(const char *name, uint8_t *buf, size_t len, int want)
{
    struct elf_image im; memset(&im, 0xAB, sizeof im);
    uint8_t *exact = malloc(len ? len : 1); if (len) memcpy(exact, buf, len);       /* exactly len bytes: ASan guards the end */
    int rc = elf_validate(exact, len, &im);
    free(exact);
    int ok = rc == want;
    if (rc != ELF_OK) { uint8_t z[sizeof im]; memset(z, 0, sizeof z); ok = ok && memcmp(&im, z, sizeof im) == 0; }   /* nothing leaks out of a refused image */
    else ok = ok && invariants(len, &im);
    if (!ok) printf("  FAIL: %s: got %d (%s), wanted %d (%s)\n", name, rc, elf_strerror(rc), want, elf_strerror(want));
    if (ok) passed++; else failed++;
    return ok;
}

/* one mutation of the valid base image */
struct neg { const char *name; int want; void (*fn)(uint8_t *b, size_t *len); };
#define PH0(b) ((b) + 64)
#define PH1(b) ((b) + 64 + 56)
static void m_len0(uint8_t *b, size_t *l) { (void)b; *l = 0; }
static void m_len10(uint8_t *b, size_t *l) { (void)b; *l = 10; }
static void m_len63(uint8_t *b, size_t *l) { (void)b; *l = 63; }
static void m_len_phtab(uint8_t *b, size_t *l) { (void)b; *l = 64 + 56 + 55; }
static void m_magic(uint8_t *b, size_t *l) { (void)l; b[1] = 'X'; }
static void m_class32(uint8_t *b, size_t *l) { (void)l; b[4] = 1; }
static void m_big(uint8_t *b, size_t *l) { (void)l; b[5] = 2; }
static void m_idver(uint8_t *b, size_t *l) { (void)l; b[6] = 2; }
static void m_ever(uint8_t *b, size_t *l) { (void)l; w32(b + 20, 2); }
static void m_osabi(uint8_t *b, size_t *l) { (void)l; b[7] = 3; }
static void m_abiver(uint8_t *b, size_t *l) { (void)l; b[8] = 1; }
static void m_pad(uint8_t *b, size_t *l) { (void)l; b[12] = 9; }
static void m_dyn(uint8_t *b, size_t *l) { (void)l; w16(b + 16, 3); }
static void m_rel(uint8_t *b, size_t *l) { (void)l; w16(b + 16, 1); }
static void m_x86(uint8_t *b, size_t *l) { (void)l; w16(b + 18, 62); }
static void m_ehsize(uint8_t *b, size_t *l) { (void)l; w16(b + 52, 52); }
static void m_phentsize(uint8_t *b, size_t *l) { (void)l; w16(b + 54, 32); }
static void m_eflags(uint8_t *b, size_t *l) { (void)l; w32(b + 48, 1); }
static void m_phnum0(uint8_t *b, size_t *l) { (void)l; w16(b + 56, 0); }
static void m_phnum9(uint8_t *b, size_t *l) { (void)l; w16(b + 56, 9); }
static void m_phnum_far(uint8_t *b, size_t *l) { w16(b + 56, 8); *l = 64 + 7 * 56; }      /* 8 headers do not fit in the file */
static void m_phoff_odd(uint8_t *b, size_t *l) { (void)l; w64(b + 32, 68); }
static void m_phoff_low(uint8_t *b, size_t *l) { (void)l; w64(b + 32, 8); }
static void m_phoff_end(uint8_t *b, size_t *l) { w64(b + 32, *l); }
static void m_phoff_wrap(uint8_t *b, size_t *l) { (void)l; w64(b + 32, 0xFFFFFFFFFFFFFFF8ULL); }
static void m_interp(uint8_t *b, size_t *l) { (void)l; w32(PH1(b), 3); }
static void m_dynamic(uint8_t *b, size_t *l) { (void)l; w32(PH1(b), 2); }
static void m_tls(uint8_t *b, size_t *l) { (void)l; w32(PH1(b), 7); }
static void m_shlib(uint8_t *b, size_t *l) { (void)l; w32(PH1(b), 5); }
static void m_unknown(uint8_t *b, size_t *l) { (void)l; w32(PH1(b), 0x70000003); }
static void m_gnustack_x(uint8_t *b, size_t *l) { (void)l; w32(PH1(b), 0x6474e551u); w32(PH1(b) + 4, PF_RX); }
static void m_off_past(uint8_t *b, size_t *l) { (void)l; w64(PH0(b) + 8, 0x10B0); }       /* congruent to the vaddr (low 12 bits 0xB0) but past the end */
static void m_filesz_past(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 32, 0x1000); w64(PH1(b) + 40, 0x1000); }
static void m_off_wrap(uint8_t *b, size_t *l) { (void)l; w64(PH0(b) + 8, 0xFFFFFFFFFFFFFFF8ULL); w64(PH0(b) + 16, ELF_VA_MIN + 0xFF8); w64(PH0(b) + 32, 16); w64(PH0(b) + 40, 16); }
static void m_memsz_lt(uint8_t *b, size_t *l) { (void)l; w64(PH0(b) + 40, 0x100); }
static void m_memsz0(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 32, 0); w64(PH1(b) + 40, 0); }
static void m_align8k(uint8_t *b, size_t *l) { (void)l; w64(PH0(b) + 48, 8192); }
static void m_align0(uint8_t *b, size_t *l) { (void)l; w64(PH0(b) + 48, 0); }
static void m_align1(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 48, 1); }
static void m_congr(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 16, DATA_VA + 8); }
static void m_below(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 16, ELF_VA_MIN - 0x1000 + 0x300); }
static void m_guard(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 16, 0x4000002000ULL + 0x300); }       /* the guard page below the stack */
static void m_stack(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 16, 0x4000003000ULL + 0x300); }       /* on top of the stack */
static void m_code_page(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 16, 0x4000000000ULL + 0x300); }  /* the fixed low code page */
static void m_atmax(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 16, ELF_VA_MAX + 0x300); }
static void m_toohigh(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 16, 0xFFFFFFFFFFFFF300ULL); }
static void m_endwrap(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 40, 0xFFFFFFFFFFFFF000ULL); }
static void m_endpast(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 16, ELF_VA_MAX - 0x1000 + 0x300); w64(PH1(b) + 40, 0x1000); }   /* one byte past the window */
static void m_overlap(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 16, TEXT_VA + 0x10); w64(PH1(b) + 8, 0xC0); }   /* shares the text segment's page */
static void m_wx(uint8_t *b, size_t *l) { (void)l; w32(PH1(b) + 4, ELF_PF_R | ELF_PF_W | ELF_PF_X); }
static void m_x_only(uint8_t *b, size_t *l) { (void)l; w32(PH0(b) + 4, ELF_PF_X); }
static void m_w_only(uint8_t *b, size_t *l) { (void)l; w32(PH1(b) + 4, ELF_PF_W); }
static void m_flagbit(uint8_t *b, size_t *l) { (void)l; w32(PH1(b) + 4, PF_RW | 8); }
static void m_pages13(uint8_t *b, size_t *l) { (void)l; w64(PH1(b) + 40, 12 * 0x1000); }                 /* 1 + 13 pages in total */
static void m_noload(uint8_t *b, size_t *l) { (void)l; w32(PH0(b), 4); w32(PH1(b), 0); }
static void m_entry_odd(uint8_t *b, size_t *l) { (void)l; w64(b + 24, TEXT_VA + 2); }
static void m_entry_data(uint8_t *b, size_t *l) { (void)l; w64(b + 24, DATA_VA); }
static void m_entry_none(uint8_t *b, size_t *l) { (void)l; w64(b + 24, ELF_VA_MIN + 0x8000); }
static void m_entry_zero(uint8_t *b, size_t *l) { (void)l; w64(b + 24, 0); }
static void m_entry_bss(uint8_t *b, size_t *l) { (void)l; w64(PH0(b) + 40, 0x400); w64(b + 24, TEXT_VA + 0x300); }   /* beyond filesz, inside memsz */
static void m_entry_tail4(uint8_t *b, size_t *l) { (void)l; w64(b + 24, TEXT_VA + 0x200); }   /* first byte after the file-backed part */

static const struct neg negs[] = {
    { "empty file", ELF_ETRUNC, m_len0 }, { "10 bytes", ELF_ETRUNC, m_len10 }, { "63 bytes", ELF_ETRUNC, m_len63 },
    { "program-header table truncated", ELF_EPHNUM, m_len_phtab },
    { "bad magic", ELF_EMAGIC, m_magic }, { "32-bit class", ELF_ECLASS, m_class32 }, { "big-endian", ELF_EDATA, m_big },
    { "ident version", ELF_EVERSION, m_idver }, { "e_version", ELF_EVERSION, m_ever }, { "OSABI", ELF_EOSABI, m_osabi },
    { "ABI version", ELF_EOSABI, m_abiver }, { "nonzero padding", ELF_EOSABI, m_pad },
    { "ET_DYN (needs relocation)", ELF_ETYPE, m_dyn }, { "ET_REL", ELF_ETYPE, m_rel }, { "x86-64", ELF_EMACHINE, m_x86 },
    { "e_ehsize", ELF_EHEADER, m_ehsize }, { "e_phentsize", ELF_EHEADER, m_phentsize }, { "e_flags", ELF_EHEADER, m_eflags },
    { "phnum 0", ELF_EPHNUM, m_phnum0 }, { "phnum 9", ELF_EPHNUM, m_phnum9 }, { "phnum beyond the file", ELF_EPHNUM, m_phnum_far },
    { "phoff misaligned", ELF_EPHNUM, m_phoff_odd }, { "phoff inside the ELF header", ELF_EPHNUM, m_phoff_low },
    { "phoff == len", ELF_EPHNUM, m_phoff_end }, { "phoff wraps", ELF_EPHNUM, m_phoff_wrap },
    { "PT_INTERP", ELF_ESEGTYPE, m_interp }, { "PT_DYNAMIC", ELF_ESEGTYPE, m_dynamic }, { "PT_TLS", ELF_ESEGTYPE, m_tls },
    { "PT_SHLIB", ELF_ESEGTYPE, m_shlib }, { "unknown type", ELF_ESEGTYPE, m_unknown }, { "executable stack", ELF_EFLAGS, m_gnustack_x },
    { "offset past the end", ELF_ESEGBOUNDS, m_off_past }, { "filesz past the end", ELF_ESEGBOUNDS, m_filesz_past }, { "offset + filesz wraps", ELF_ESEGBOUNDS, m_off_wrap },
    { "memsz < filesz", ELF_ESEGSIZE, m_memsz_lt }, { "memsz 0", ELF_ESEGSIZE, m_memsz0 },
    { "align 8192", ELF_EALIGN, m_align8k }, { "align 0", ELF_EALIGN, m_align0 }, { "align 1", ELF_EALIGN, m_align1 }, { "vaddr/offset not congruent", ELF_EALIGN, m_congr },
    { "below the window", ELF_ERANGE, m_below }, { "on the guard page", ELF_ERANGE, m_guard }, { "on the stack", ELF_ERANGE, m_stack },
    { "on the fixed code page", ELF_ERANGE, m_code_page }, { "at the window end", ELF_ERANGE, m_atmax }, { "vaddr near 2^64", ELF_ERANGE, m_toohigh },
    { "memsz wraps the address space", ELF_ERANGE, m_endwrap }, { "one byte past the window", ELF_ERANGE, m_endpast },
    { "two segments share a page", ELF_EOVERLAP, m_overlap },
    { "writable and executable", ELF_EWX, m_wx }, { "execute-only (no PF_R)", ELF_EFLAGS, m_x_only }, { "write-only", ELF_EFLAGS, m_w_only }, { "unknown flag bit", ELF_EFLAGS, m_flagbit },
    { "13 pages", ELF_ETOOBIG, m_pages13 },
    { "no PT_LOAD", ELF_ENOLOAD, m_noload },
    { "entry misaligned", ELF_EENTRY, m_entry_odd }, { "entry in a non-executable segment", ELF_EENTRY, m_entry_data }, { "entry in no segment", ELF_EENTRY, m_entry_none },
    { "entry 0", ELF_EENTRY, m_entry_zero }, { "entry in bss of the code segment", ELF_EENTRY, m_entry_bss },
    { "entry just past the file-backed bytes", ELF_EENTRY, m_entry_tail4 },
};

/* a deterministic PRNG (no libc rand: reproducible everywhere) */
static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

int main(void)
{
    init_base();
    printf("== positive cases ==\n");
    {
        uint8_t *b = build(IMG_LEN, TEXT_VA, base_ph, 2);
        struct elf_image im; int rc = elf_validate(b, IMG_LEN, &im);
        CHECK("valid image accepted", rc == ELF_OK);
        CHECK("entry reported", im.entry == TEXT_VA);
        CHECK("two segments, 2 pages (text in page 0; data and its bss in page 1)", im.nseg == 2 && im.pages == 2);
        CHECK("segment 0 is R+X with the right file range", im.seg[0].flags == PF_RX && im.seg[0].offset == 0xB0 && im.seg[0].filesz == 0x200 && im.seg[0].memsz == 0x200 && im.seg[0].vaddr == TEXT_VA);
        CHECK("segment 1 is R+W with bss (memsz > filesz)", im.seg[1].flags == PF_RW && im.seg[1].filesz == 0x100 && im.seg[1].memsz == 0x900);
        CHECK("invariants hold", invariants(IMG_LEN, &im));
        free(b);
    }
    {
        /* PT_NOTE and a harmless PT_GNU_STACK are ignored; a trailing PT_NULL too */
        struct ph p[5] = { { 4, 4, 0, 0, 0, 0, 4 }, { PT_LOAD_, PF_RX, 0xE8, ELF_VA_MIN + 0xE8, 0x100, 0x100, ELF_PAGE }, { 0x6474e551u, PF_RW, 0, 0, 0, 0, 16 }, { PT_LOAD_, PF_RW, 0x300, ELF_VA_MIN + 0x2300, 0x80, 0x80, ELF_PAGE }, { 0, 0, 0, 0, 0, 0, 0 } };
        uint8_t *b = build(IMG_LEN, ELF_VA_MIN + 0xE8, p, 5);
        struct elf_image im; int rc = elf_validate(b, IMG_LEN, &im);
        CHECK("PT_NOTE / PT_GNU_STACK(RW) / PT_NULL are ignored", rc == ELF_OK && im.nseg == 2 && im.pages == 2);
        free(b);
    }
    {
        /* exactly 12 pages is the limit; a read-only segment is allowed; a segment may start mid-page */
        struct ph p[2] = { { PT_LOAD_, ELF_PF_R | ELF_PF_X, 0xB0, ELF_VA_MIN + 0xB0, 0x200, 0x200, ELF_PAGE }, { PT_LOAD_, ELF_PF_R, 0x300, ELF_VA_MIN + 0x1300, 0x100, 11 * 0x1000 - 0x300, ELF_PAGE } };
        uint8_t *b = build(IMG_LEN, ELF_VA_MIN + 0xB0, p, 2);
        struct elf_image im; int rc = elf_validate(b, IMG_LEN, &im);
        CHECK("exactly 12 pages accepted", rc == ELF_OK && im.pages == 12);
        free(b);
        p[1].memsz += 0x1000;
        b = build(IMG_LEN, ELF_VA_MIN + 0xB0, p, 2);
        CHECK("13 pages refused", elf_validate(b, IMG_LEN, &im) == ELF_ETOOBIG);
        free(b);
    }
    {
        /* the largest allowed file is accepted, one byte more is not */
        uint8_t *b = build(ELF_MAX_FILE, TEXT_VA, base_ph, 2); struct elf_image im;
        CHECK("file of exactly ELF_MAX_FILE accepted", elf_validate(b, ELF_MAX_FILE, &im) == ELF_OK);
        free(b);
        b = build(ELF_MAX_FILE + 1, TEXT_VA, base_ph, 2);
        CHECK("file of ELF_MAX_FILE + 1 refused", elf_validate(b, ELF_MAX_FILE + 1, &im) == ELF_ETOOBIG);
        free(b);
    }
    {
        uint8_t *b = build(IMG_LEN, TEXT_VA + 0x1FC, base_ph, 2); struct elf_image im;
        CHECK("entry on the LAST instruction of the file-backed bytes is accepted", elf_validate(b, IMG_LEN, &im) == ELF_OK && im.entry == TEXT_VA + 0x1FC);
        free(b);
    }
    CHECK("NULL arguments refused without a crash", elf_validate(0, 100, &(struct elf_image){ 0 }) == ELF_ETRUNC && elf_validate((const uint8_t *)"x", 1, 0) == ELF_ETRUNC);

    printf("== %zu negative cases ==\n", sizeof negs / sizeof *negs);
    for (size_t i = 0; i < sizeof negs / sizeof *negs; i++) {
        uint8_t *b = build(IMG_LEN, TEXT_VA, base_ph, 2); size_t len = IMG_LEN;
        negs[i].fn(b, &len);
        try(negs[i].name, b, len, negs[i].want);
        free(b);
    }
    {   /* five PT_LOAD headers */
        struct ph p[5]; for (int i = 0; i < 5; i++) p[i] = (struct ph){ PT_LOAD_, PF_RW, 0x300, ELF_VA_MIN + 0x1000 * (uint64_t)i + 0x300, 0x10, 0x10, ELF_PAGE };
        p[0].flags = PF_RX; uint8_t *b = build(IMG_LEN, ELF_VA_MIN + 0x300, p, 5);
        try("five PT_LOAD segments", b, IMG_LEN, ELF_EPHNUM); free(b);
    }
    {   /* a file larger than the filesystem can hold */
        uint8_t *b = build(ELF_MAX_FILE + 1, TEXT_VA, base_ph, 2);
        try("oversized file", b, ELF_MAX_FILE + 1, ELF_ETOOBIG); free(b);
    }

    printf("== fuzzing ==\n");
    {
        int accepted = 0, refused = 0; const int N = 300000;
        for (int it = 0; it < N; it++) {
            size_t len = IMG_LEN;
            uint8_t *b = build(len, TEXT_VA, base_ph, 2);
            int kind = (int)(rnd() % 4);
            if (kind == 0) { int n = 1 + (int)(rnd() % 4); while (n--) b[rnd() % 300] ^= (uint8_t)(1u << (rnd() % 8)); }       /* bit flips in the headers */
            else if (kind == 1) { len = rnd() % (IMG_LEN + 1); }                                                                /* truncation */
            else if (kind == 2) { int n = 1 + (int)(rnd() % 3); while (n--) { size_t o = 24 + rnd() % 200; b[o] = (uint8_t)rnd(); } }   /* byte overwrites */
            else { size_t o = (rnd() & 1) ? 64 + (rnd() % 2) * 56 + (rnd() % 7) * 8 : 24 + (rnd() % 4) * 8; uint64_t v = ((uint64_t)rnd() << 32 | rnd()) >> (rnd() % 64); if (o + 8 <= len) w64(b + o, v); }   /* 64-bit fields to arbitrary values */
            uint8_t *exact = malloc(len ? len : 1); if (len) memcpy(exact, b, len);
            struct elf_image im; int rc = elf_validate(exact, len, &im);
            if (rc == ELF_OK) { accepted++; if (!invariants(len, &im)) { failed++; printf("  FAIL: fuzz iteration %d accepted an image that violates an invariant\n", it); } }
            else { refused++; uint8_t z[sizeof im]; memset(z, 0, sizeof z); if (memcmp(&im, z, sizeof im)) { failed++; printf("  FAIL: fuzz iteration %d leaked data on refusal\n", it); } }
            free(exact); free(b);
        }
        printf("  fuzz: %d iterations, %d accepted (all invariants held), %d refused\n", N, accepted, refused);
        CHECK("fuzzing reached both outcomes", accepted > 100 && refused > 100);
    }

    printf("\n== Summary ==\nPassed: %d\nFailed: %d\n", passed, failed);
    return failed != 0;
}
