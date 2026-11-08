/*
 * kernel/mm/selftest.c — in-kernel memory-management tests.
 *
 * Tests run on real (emulated) hardware state: the page allocator is
 * exercised directly, and MMU behaviour is checked with the hardware
 * address-translation instruction (AT), which reports a fault via PAR_EL1
 * without raising an exception. Genuine faults are raised only by the
 * separate AURORA_FAULT_TEST builds at the bottom of this file, so normal
 * boot tests never depend on a crash.
 */
#include <arch.h>
#include <console.h>
#include <mm.h>
#include <mmpolicy.h>
#include <selftest.h>

const struct fdt_info *boot_fdt_info(void);
uint64_t boot_dtb_address(void);

static int npass, nfail;

static void check(const char *name, int ok)
{
    console_puts("[test] "); console_puts(name); console_puts(ok ? ": PASS\n" : ": FAIL\n");
    if (ok) npass++; else nfail++;
}

void selftest_pmm(void)
{
    struct pmm_stats s0, s1;
    pmm_get_stats(&s0);

    uintptr_t p = pmm_alloc_page();
    pmm_get_stats(&s1);
    check("pmm: alloc returns a page-aligned page inside RAM", p && !(p & (PAGE_SIZE - 1)) && pmm_in_ram(p));
    check("pmm: alloc never returns kernel image or boot region", p >= (uintptr_t)__kernel_end);
    check("pmm: alloc updates accounting", s1.free_pages == s0.free_pages - 1 && s1.allocated_pages == s0.allocated_pages + 1);
    int zero = 1;
    for (uint64_t i = 0; i < PAGE_SIZE / 8; i++) if (((volatile uint64_t *)p)[i]) zero = 0;
    check("pmm: allocated page is zeroed", zero);
    check("pmm: allocated page reports allocated, not reserved", pmm_is_allocated(p) && !pmm_is_reserved(p));
    ((volatile uint64_t *)p)[0] = 0xA5A5A5A5DEADBEEFULL;
    check("pmm: free succeeds", pmm_free_page(p) == PMM_OK);
    pmm_get_stats(&s1);
    check("pmm: free restores accounting", s1.free_pages == s0.free_pages && s1.allocated_pages == s0.allocated_pages);

    check("pmm: double free detected", pmm_free_page(p) == PMM_ERR_DOUBLE);
    check("pmm: unaligned free rejected", pmm_free_page(p + 1) == PMM_ERR_ALIGN);
    check("pmm: free below RAM rejected", pmm_free_page(0) == PMM_ERR_RANGE);
    check("pmm: free above RAM rejected", pmm_free_page(pmm_ram_end()) == PMM_ERR_RANGE);
    check("pmm: free of arbitrary address rejected", pmm_free_page(0xDEAD0000UL) == PMM_ERR_RANGE);
    check("pmm: free of kernel text page rejected", pmm_free_page((uintptr_t)__text_start) == PMM_ERR_RESERVED);
    check("pmm: free of boot-region page rejected", pmm_free_page(pmm_ram_base()) == PMM_ERR_RESERVED);
    uintptr_t never = pmm_ram_end() - PAGE_SIZE;
    check("pmm: free of never-allocated page rejected", !pmm_is_allocated(never) && !pmm_is_reserved(never) && pmm_free_page(never) == PMM_ERR_DOUBLE);
    pmm_get_stats(&s1);
    check("pmm: rejected frees counted and do not change free pages", s1.free_errors == s0.free_errors + 8 && s1.free_pages == s0.free_pages);

    check("pmm: kernel image start reserved", pmm_is_reserved((uintptr_t)__image_start));
    check("pmm: last kernel image byte reserved", pmm_is_reserved((uintptr_t)__kernel_end - 1));
    check("pmm: stack pages reserved", pmm_is_reserved((uintptr_t)__stack_top - 1) && pmm_is_reserved((uintptr_t)__stack_guard));
    check("pmm: page after kernel image not reserved", !pmm_is_reserved((uintptr_t)__kernel_end));

    check("pmm: reserve_range rejects zero size and address overflow", pmm_reserve_range(pmm_ram_base(), 0) == -3 && pmm_reserve_range(~0ULL - 10, 100) == -3);
    uint64_t rsv0 = s0.reserved_pages;
    check("pmm: reservation entirely outside RAM is ignored, not an error", pmm_reserve_range(0x1000, 0x1000) == 0 && pmm_reserve_range(pmm_ram_end(), PAGE_SIZE) == 0);
    pmm_get_stats(&s1);
    check("pmm: ignored reservations do not change accounting", s1.reserved_pages == rsv0 && s1.free_pages == s0.free_pages);
    check("pmm: reserving the already-reserved kernel image again is idempotent", pmm_reserve_range((uint64_t)(uintptr_t)__image_start, PAGE_SIZE) == 0 && (pmm_get_stats(&s1), s1.reserved_pages == rsv0));

    uintptr_t q = pmm_alloc_page();
    check("pmm: cannot reserve an already allocated page", q && pmm_reserve(q, q + 1) < 0 && pmm_is_allocated(q));
    pmm_free_page(q);
}

void selftest_mmu(void)
{
    uint64_t pa = 0;
    uint64_t sctlr = read_sctlr_el1();
    check("mmu: SCTLR_EL1 has M, C, I and WXN set", vmm_mmu_enabled() && (sctlr & (1ULL << 2)) && (sctlr & (1ULL << 12)) && (sctlr & (1ULL << 19)));
    check("mmu: running at EL1", read_current_el() == 1);
    check("mmu: UART still works after enabling the MMU", 1);   /* this line itself is printed over the UART */

    check("mmu: text is mapped readable and identity-mapped", vmm_hw_translate((uint64_t)(uintptr_t)__text_start, 0, &pa) == 0 && pa == (uint64_t)(uintptr_t)__text_start);
    check("mmu: text is NOT writable (W^X)", vmm_hw_translate((uint64_t)(uintptr_t)__text_start, 1, 0) == -1);
    check("mmu: rodata is readable but NOT writable", vmm_hw_translate((uint64_t)(uintptr_t)__rodata_start, 0, 0) == 0 && vmm_hw_translate((uint64_t)(uintptr_t)__rodata_start, 1, 0) == -1);
    check("mmu: data is readable and writable", vmm_hw_translate((uint64_t)(uintptr_t)__data_start, 1, &pa) == 0 && pa == (uint64_t)(uintptr_t)__data_start);
    check("mmu: stack is mapped", vmm_hw_translate((uint64_t)(uintptr_t)__stack_top - 8, 1, 0) == 0);
    check("mmu: stack guard page is unmapped", vmm_hw_translate((uint64_t)(uintptr_t)__stack_guard, 0, 0) == -1);
    check("mmu: UART window is mapped as device", vmm_hw_translate(0x09000000ULL, 1, &pa) == 0 && pa == 0x09000000ULL);
    check("mmu: unmapped address 0xDEAD0000 would fault", vmm_hw_translate(0xDEAD0000ULL, 0, 0) == -1);
    check("mmu: address 0 would fault", vmm_hw_translate(0, 0, 0) == -1);
    {   /* device-tree no-map regions must be unmapped; ordinary reserved regions stay mapped. Vacuously true with none. */
        const struct fdt_info *fi = boot_fdt_info(); int ok = 1;
        for (int i = 0; i < fi->nreserved; i++) {
            uint64_t a = fi->reserved[i].addr & ~(PAGE_SIZE - 1);
            if (!pmm_in_ram(a) || fi->reserved[i].addr < pmm_ram_base()) continue;
            int mapped = vmm_hw_translate(a, 0, 0) == 0;
            if (fi->reserved[i].nomap == mapped) ok = 0;
        }
        check("mmu: no-map regions are unmapped, other reserved regions stay mapped", ok);
    }
    check("mmu: bad-VA (>= 2^39) would fault", vmm_hw_translate(1ULL << 40, 0, 0) == -1);

    /* Runtime mapping: alias a fresh page at an unused virtual address and verify the translation. */
    uintptr_t page = pmm_alloc_page();
    const uint64_t alias = 0x80000000ULL;
    int r = page ? vmm_map(alias, page, PAGE_SIZE, VMM_ATTR_NORMAL_RW) : -99;
    check("vmm: map a page at a new virtual address", r == 0);
    if (r == 0) {
        *(volatile uint64_t *)alias = 0x1122334455667788ULL;
        check("vmm: write through alias is visible at the physical page", *(volatile uint64_t *)page == 0x1122334455667788ULL);
        check("vmm: hardware translation of alias equals physical page", vmm_hw_translate(alias, 1, &pa) == 0 && pa == page);
    }
    check("vmm: remapping an existing virtual address is refused", vmm_map(alias, page, PAGE_SIZE, VMM_ATTR_NORMAL_RW) != 0);
    check("vmm: unaligned map is refused", vmm_map(alias + 0x10000 + 1, page, PAGE_SIZE, VMM_ATTR_NORMAL_RW) != 0);
    check("vmm: map beyond the 39-bit address space is refused", vmm_map(1ULL << 39, page, PAGE_SIZE, VMM_ATTR_NORMAL_RW) != 0);
    check("vmm: invalid attribute is refused", vmm_map(alias + 0x20000, page, PAGE_SIZE, 99) != 0);

    /* ---- vmm_map is all-or-nothing ---- */
    {
        uint64_t base = 0xA0000000ULL, dummy;
        uintptr_t pg = pmm_alloc_page();
        check("vmm: single page mapping at a fresh address succeeds", pg && vmm_map(base + 0x1000, pg, PAGE_SIZE, VMM_ATTR_NORMAL_RW) == VMM_OK);
        /* [base, base+3 pages) overlaps the existing page in the MIDDLE: nothing may be installed. */
        int r2 = vmm_map(base, pg, 3 * PAGE_SIZE, VMM_ATTR_NORMAL_RW);
        check("vmm: overlap with an existing page is refused with CONFLICT", r2 == VMM_ERR_CONFLICT);
        check("vmm: failed map left the page BEFORE the conflict unmapped", vmm_hw_translate(base, 0, &dummy) == -1);
        check("vmm: failed map left the page AFTER the conflict unmapped", vmm_hw_translate(base + 0x2000, 0, &dummy) == -1);
        check("vmm: the pre-existing page is untouched", vmm_hw_translate(base + 0x1000, 1, &dummy) == 0 && dummy == pg);

        /* Existing 2 MiB block: a request that starts on an empty page and runs into it must install nothing. */
        check("vmm: 2 MiB block mapping succeeds", vmm_map(0x140000000ULL, 0x40200000ULL, 0x200000ULL, VMM_ATTR_NORMAL_RO) == VMM_OK);
        int r3 = vmm_map(0x140000000ULL - PAGE_SIZE, pg, 2 * PAGE_SIZE, VMM_ATTR_NORMAL_RW);
        check("vmm: running into an existing block is refused with CONFLICT", r3 == VMM_ERR_CONFLICT);
        check("vmm: failed map left the empty page before the block unmapped", vmm_hw_translate(0x140000000ULL - PAGE_SIZE, 0, &dummy) == -1);
        check("vmm: the block was not split or altered", vmm_hw_translate(0x140000000ULL + 0x1000, 0, &dummy) == 0 && vmm_hw_translate(0x140000000ULL + 0x1000, 1, 0) == -1);
        check("vmm: mapping over the identity-mapped RAM blocks is refused", vmm_map(pmm_ram_end() - 0x200000ULL, pg, PAGE_SIZE, VMM_ATTR_NORMAL_RW) == VMM_ERR_CONFLICT);

        check("vmm: virtual address overflow is refused", vmm_map(~0ULL - 0xFFF, pg, 2 * PAGE_SIZE, VMM_ATTR_NORMAL_RW) == VMM_ERR_RANGE);
        check("vmm: range ending past the 39-bit space is refused", vmm_map((1ULL << 39) - PAGE_SIZE, pg, 2 * PAGE_SIZE, VMM_ATTR_NORMAL_RW) == VMM_ERR_RANGE);
        check("vmm: physical address overflow is refused", vmm_map(0xB0000000ULL, ~0ULL - 0xFFF, 2 * PAGE_SIZE, VMM_ATTR_NORMAL_RW) == VMM_ERR_RANGE);
        check("vmm: physical address beyond the implemented width is refused", vmm_map(0xB0000000ULL, 1ULL << 47, PAGE_SIZE, VMM_ATTR_NORMAL_RW) == VMM_ERR_RANGE);
        check("vmm: zero size and bad attribute are refused", vmm_map(0xB0000000ULL, pg, 0, VMM_ATTR_NORMAL_RW) == VMM_ERR_ARG && vmm_map(0xB0000000ULL, pg, PAGE_SIZE, 0) == VMM_ERR_ARG);
        check("vmm: refused requests mapped nothing", vmm_hw_translate(0xB0000000ULL, 0, &dummy) == -1 && vmm_hw_translate((1ULL << 39) - PAGE_SIZE, 0, &dummy) == -1);
    }

    /* ---- physical-target policy: aliases through DIFFERENT virtual addresses ---- */
    {
        uint64_t d, va = 0x1C0000000ULL;                 /* an unused 1 GiB slot, so these never collide with identity maps */
        const uint64_t text0 = (uint64_t)(uintptr_t)__text_start, text_end = (uint64_t)(uintptr_t)__text_end;
        const uint64_t ro0 = (uint64_t)(uintptr_t)__rodata_start, ro_end = (uint64_t)(uintptr_t)__rodata_end;
        const uint64_t data0 = (uint64_t)(uintptr_t)__data_start;
        const uint64_t ram_end = pmm_ram_end(), uart = 0x09000000ULL;
        struct pmm_stats pa_, pb_;
        uintptr_t freepg = pmm_alloc_page();            /* a normal RAM page for control cases */

        /* Rejected requests first: they must change neither accounting nor translations. */
        pmm_get_stats(&pa_);
        int r1 = vmm_map(va + 0x0000, text0, PAGE_SIZE, VMM_ATTR_NORMAL_RW);       int d1 = vmm_last_denial();
        int r2 = vmm_map(va + 0x1000, text_end - PAGE_SIZE, PAGE_SIZE, VMM_ATTR_NORMAL_RW);
        int r3 = vmm_map(va + 0x2000, ro0, PAGE_SIZE, VMM_ATTR_NORMAL_RW);         int d3 = vmm_last_denial();
        int r4 = vmm_map(va + 0x3000, ro_end - PAGE_SIZE, PAGE_SIZE, VMM_ATTR_NORMAL_RW);
        int r5 = vmm_map(va + 0x10000, text0 - PAGE_SIZE, 2 * PAGE_SIZE, VMM_ATTR_NORMAL_RW);   /* boot-region page + text page */
        int d5 = vmm_last_denial();
        int r6 = vmm_map(va + 0x20000, text_end - PAGE_SIZE, 2 * PAGE_SIZE, VMM_ATTR_NORMAL_RW); /* text page + rodata page */
        int r7 = vmm_map(va + 0x30000, data0, PAGE_SIZE, VMM_ATTR_NORMAL_RX);      int d7 = vmm_last_denial();
        int r8 = vmm_map(va + 0x31000, ro0, PAGE_SIZE, VMM_ATTR_NORMAL_RX);
        int r9 = vmm_map(va + 0x32000, freepg, PAGE_SIZE, VMM_ATTR_NORMAL_RX);
        int r10 = vmm_map(va + 0x33000, text0, text_end - text0 + PAGE_SIZE, VMM_ATTR_NORMAL_RX);   /* text + first rodata page */
        int r11 = vmm_map(va + 0x40000, uart, PAGE_SIZE, VMM_ATTR_NORMAL_RW);      int d11 = vmm_last_denial();
        int r12 = vmm_map(va + 0x41000, ram_end, PAGE_SIZE, VMM_ATTR_NORMAL_RW);   int d12 = vmm_last_denial();
        int r13 = vmm_map(va + 0x42000, ram_end - PAGE_SIZE, 2 * PAGE_SIZE, VMM_ATTR_NORMAL_RW);   /* crosses the end of RAM */
        int r14 = vmm_map(va + 0x50000, freepg, PAGE_SIZE, VMM_ATTR_DEVICE);       int d14 = vmm_last_denial();
        int r15 = vmm_map(va + 0x51000, 0x0a000000ULL, PAGE_SIZE, VMM_ATTR_DEVICE); int d15 = vmm_last_denial();
        int r16 = vmm_map(va + 0x52000, uart, 2 * PAGE_SIZE, VMM_ATTR_DEVICE);     /* runs past the 1-page UART window */
        pmm_get_stats(&pb_);

        check("policy: RW alias of the first kernel-text page is refused (TEXT_WRITE)", r1 == VMM_ERR_POLICY && d1 == MMP_DENY_TEXT_WRITE);
        check("policy: RW alias of the last kernel-text page is refused", r2 == VMM_ERR_POLICY);
        check("policy: RW alias of the first rodata page is refused (RODATA_WRITE)", r3 == VMM_ERR_POLICY && d3 == MMP_DENY_RODATA_WRITE);
        check("policy: RW alias of the last rodata page is refused", r4 == VMM_ERR_POLICY);
        check("policy: mixed request (unprotected page + text page) is refused as a whole", r5 == VMM_ERR_POLICY && d5 == MMP_DENY_TEXT_WRITE);
        check("policy: request crossing the text/rodata boundary is refused", r6 == VMM_ERR_POLICY);
        check("policy: executable alias of a data page is refused (W^X across aliases)", r7 == VMM_ERR_POLICY && d7 == MMP_DENY_EXEC_OUTSIDE_TEXT);
        check("policy: executable alias of rodata or free RAM is refused", r8 == VMM_ERR_POLICY && r9 == VMM_ERR_POLICY);
        check("policy: executable request extending past the end of text is refused", r10 == VMM_ERR_POLICY);
        check("policy: cacheable mapping of UART MMIO is refused (NORMAL_ON_MMIO)", r11 == VMM_ERR_POLICY && d11 == MMP_DENY_NORMAL_ON_MMIO);
        check("policy: cacheable mapping outside or crossing the end of RAM is refused", r12 == VMM_ERR_POLICY && d12 == MMP_DENY_NORMAL_OUTSIDE_RAM && r13 == VMM_ERR_POLICY);
        check("policy: device attributes on RAM are refused (DEVICE_ON_RAM)", r14 == VMM_ERR_POLICY && d14 == MMP_DENY_DEVICE_ON_RAM);
        check("policy: device mapping outside a registered window is refused", r15 == VMM_ERR_POLICY && d15 == MMP_DENY_DEVICE_OUTSIDE_WINDOW && r16 == VMM_ERR_POLICY);
        int any = 0;
        for (uint64_t off = 0; off < 0x53000; off += PAGE_SIZE) if (vmm_hw_translate(va + off, 0, &d) == 0) any = 1;
        check("policy: no refused request installed any mapping (hardware translation of every alias VA faults)", !any);
        check("policy: refused requests changed no allocation accounting", pa_.free_pages == pb_.free_pages && pa_.allocated_pages == pb_.allocated_pages);
        check("policy: the original kernel-text and rodata mappings are still read-only", vmm_hw_translate(text0, 1, 0) == -1 && vmm_hw_translate(ro0, 1, 0) == -1 && vmm_hw_translate(text0, 0, 0) == 0);
        check("policy: unaligned and overflowing physical ranges are rejected before policy", vmm_map(va + 0x60000, text0 + 1, PAGE_SIZE, VMM_ATTR_NORMAL_RO) == VMM_ERR_ALIGN && vmm_map(va + 0x60000, ~0ULL - 0xFFF, 2 * PAGE_SIZE, VMM_ATTR_NORMAL_RO) == VMM_ERR_RANGE);

        /* Legitimate mappings still work, and hardware translation agrees with the policy. */
        int a1 = vmm_map(va + 0x70000, text0, PAGE_SIZE, VMM_ATTR_NORMAL_RO);
        int a2 = vmm_map(va + 0x71000, text0, text_end - text0, VMM_ATTR_NORMAL_RX);
        int a3 = vmm_map(va + 0x80000, ro0, PAGE_SIZE, VMM_ATTR_NORMAL_RO);
        int a4 = vmm_map(va + 0x81000, data0, PAGE_SIZE, VMM_ATTR_NORMAL_RW);
        int a5 = vmm_map(va + 0x82000, freepg, PAGE_SIZE, VMM_ATTR_NORMAL_RW);
        int a6 = vmm_map(va + 0x83000, uart, PAGE_SIZE, VMM_ATTR_DEVICE);
        check("policy: read-only aliases of text and rodata are allowed and read-only in hardware",
              a1 == VMM_OK && a3 == VMM_OK && vmm_hw_translate(va + 0x70000, 0, &d) == 0 && d == text0 &&
              vmm_hw_translate(va + 0x70000, 1, 0) == -1 && vmm_hw_translate(va + 0x80000, 0, &d) == 0 && d == ro0 && vmm_hw_translate(va + 0x80000, 1, 0) == -1);
        check("policy: an executable alias of exactly the text range is allowed", a2 == VMM_OK && vmm_hw_translate(va + 0x71000, 0, &d) == 0 && d == text0);
        check("policy: writable aliases of data and free RAM are allowed", a4 == VMM_OK && a5 == VMM_OK && vmm_hw_translate(va + 0x81000, 1, &d) == 0 && d == data0 && vmm_hw_translate(va + 0x82000, 1, &d) == 0 && d == freepg);
        check("policy: a device alias of the registered UART window is allowed", a6 == VMM_OK && vmm_hw_translate(va + 0x83000, 1, &d) == 0 && d == uart);
        check("policy: MMIO windows can be registered, but never over RAM", vmm_register_mmio(pmm_ram_base(), PAGE_SIZE) == VMM_ERR_POLICY && vmm_register_mmio(0x0a000000ULL + 1, PAGE_SIZE) == VMM_ERR_ALIGN &&
              vmm_register_mmio(0x0a000000ULL, PAGE_SIZE) == VMM_OK && vmm_map(va + 0x84000, 0x0a000000ULL, PAGE_SIZE, VMM_ATTR_DEVICE) == VMM_OK);
        pmm_free_page(freepg);

        /* Device-tree no-map memory: never mappable by any attribute, alone or as part of a larger request. */
        const struct fdt_info *fi = boot_fdt_info(); int nm_ok = 1, nm_tested = 0;
        for (int i = 0; i < fi->nreserved; i++) {
            if (!fi->reserved[i].nomap) continue;
            uint64_t base = fi->reserved[i].addr & ~(PAGE_SIZE - 1); nm_tested++;
            static const int attrs[] = { VMM_ATTR_NORMAL_RW, VMM_ATTR_NORMAL_RO, VMM_ATTR_NORMAL_RX, VMM_ATTR_DEVICE };
            for (int k = 0; k < 4; k++) {
                uint64_t v = va + 0x100000 + (uint64_t)(i * 8 + k) * 0x4000;
                if (vmm_map(v, base, PAGE_SIZE, attrs[k]) != VMM_ERR_POLICY || vmm_last_denial() != MMP_DENY_NOMAP) nm_ok = 0;
                if (vmm_map(v + 0x1000, base - PAGE_SIZE, 2 * PAGE_SIZE, attrs[k]) != VMM_ERR_POLICY) nm_ok = 0;   /* starts in the neighbouring page */
                if (vmm_hw_translate(v, 0, &d) == 0 || vmm_hw_translate(v + 0x1000, 0, &d) == 0 || vmm_hw_translate(v + 0x2000, 0, &d) == 0) nm_ok = 0;
            }
        }
        console_puts("[info] no-map regions policy-tested: "); console_dec((uint64_t)nm_tested); console_puts("\n");
        check("policy: no-map memory is refused for every attribute, also inside mixed requests, and nothing is installed", nm_ok);
    }

    /* Exhaustion, with the MMU on: every free page is handed out exactly once, then 0. */
    struct pmm_stats s0, s1;
    pmm_get_stats(&s0);
    const struct fdt_info *fi = boot_fdt_info();
    uint64_t dtb_base = boot_dtb_address();
    uintptr_t head = 0; uint64_t n = 0, bad = 0, dt_bad = 0;
    for (;;) {
        uintptr_t p = pmm_alloc_page();
        if (!p) break;
        if ((p & (PAGE_SIZE - 1)) || !pmm_in_ram(p) || pmm_is_reserved(p) || p < (uintptr_t)__kernel_end) bad++;
        for (int i = 0; i < fi->nreserved; i++)
            if (p + PAGE_SIZE > fi->reserved[i].addr && p < fi->reserved[i].addr + fi->reserved[i].size) dt_bad++;
        if (fi->valid && p + PAGE_SIZE > dtb_base && p < dtb_base + fi->totalsize) dt_bad++;
        *(volatile uint64_t *)p = head; head = p; n++;
    }
    pmm_get_stats(&s1);
    check("pmm: exhaustion hands out exactly the free pages", n == s0.free_pages && s1.free_pages == 0);
    check("pmm: exhaustion never hands out reserved or out-of-range pages", bad == 0);
    check("pmm: exhaustion never hands out the device-tree blob or its reserved regions", dt_bad == 0);
    check("pmm: exhausted allocator returns 0 and counts the failure", pmm_alloc_page() == 0 && s1.alloc_failures == s0.alloc_failures + 1);
    uint64_t freed_bad = 0;
    while (head) { uintptr_t next = *(volatile uint64_t *)head; if (pmm_free_page(head) != PMM_OK) freed_bad++; head = next; }
    pmm_get_stats(&s1);
    check("pmm: freeing everything restores the free count", freed_bad == 0 && s1.free_pages == s0.free_pages);

    /* Page-table allocation failure: no memory for the tables => clean refusal, nothing mapped, recoverable. */
    {
        uint64_t dummy; const uint64_t far_va = 0x4000000000ULL;   /* empty 1 GiB slot: needs an L2 and an L3 table */
        uintptr_t keep = pmm_alloc_page(); uintptr_t h = 0; uint64_t cnt = 0;
        for (;;) { uintptr_t p = pmm_alloc_page(); if (!p) break; *(volatile uint64_t *)p = h; h = p; cnt++; }
        uintptr_t spare = h; h = *(volatile uint64_t *)h; cnt--; pmm_free_page(spare);     /* exactly one free page left */
        struct pmm_stats a, b; pmm_get_stats(&a);
        int r = vmm_map(far_va, keep, PAGE_SIZE, VMM_ATTR_NORMAL_RW);
        pmm_get_stats(&b);
        check("vmm: page-table allocation failure is reported as NOMEM", r == VMM_ERR_NOMEM);
        check("vmm: NOMEM leaves nothing mapped and consumes no page", vmm_hw_translate(far_va, 0, &dummy) == -1 && a.free_pages == 1 && b.free_pages == 1);
        while (h) { uintptr_t nx = *(volatile uint64_t *)h; pmm_free_page(h); h = nx; }
        check("vmm: the same request succeeds once memory is available again", vmm_map(far_va, keep, PAGE_SIZE, VMM_ATTR_NORMAL_RW) == VMM_OK && vmm_hw_translate(far_va, 1, &dummy) == 0 && dummy == keep);
        (void)cnt;
    }
}

int selftest_summary(void)
{
    console_puts("AURORA-MM-SELFTEST: "); console_dec((uint64_t)npass); console_puts(" passed, ");
    console_dec((uint64_t)nfail); console_puts(" failed\n");
    return nfail;
}

#ifdef AURORA_FAULT_TEST
/* Deliberate faults: each build triggers exactly one genuine hardware exception. */
void selftest_fault(void)
{
    if (AURORA_FAULT_TEST >= 6) return;            /* cases 6 and 7 live in irqtest.c */
    console_puts("[fault-test] case "); console_dec(AURORA_FAULT_TEST); console_puts(": ");
    switch (AURORA_FAULT_TEST) {
    case 1: console_puts("read unmapped 0xDEAD0000\n");   (void)*(volatile uint32_t *)0xDEAD0000UL; break;
    case 2: console_puts("write to kernel text\n");       *(volatile uint32_t *)__text_start = 0; break;
    case 3: console_puts("execute from data (W^X)\n");    ((void (*)(void))(uintptr_t)__data_start)(); break;
    case 4: console_puts("touch stack guard page\n");     (void)*(volatile uint64_t *)__stack_guard; break;
    case 5: console_puts("write to rodata\n");            *(volatile uint32_t *)__rodata_start = 0; break;
    default: console_puts("unknown case\n"); return;
    }
    console_puts("ERROR: fault test did not fault!\n");
}
#else
void selftest_fault(void) {}
#endif
