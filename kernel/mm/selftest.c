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
#include <selftest.h>

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

    /* Exhaustion, with the MMU on: every free page is handed out exactly once, then 0. */
    struct pmm_stats s0, s1;
    pmm_get_stats(&s0);
    uintptr_t head = 0; uint64_t n = 0, bad = 0;
    for (;;) {
        uintptr_t p = pmm_alloc_page();
        if (!p) break;
        if ((p & (PAGE_SIZE - 1)) || !pmm_in_ram(p) || pmm_is_reserved(p) || p < (uintptr_t)__kernel_end) bad++;
        *(volatile uint64_t *)p = head; head = p; n++;
    }
    pmm_get_stats(&s1);
    check("pmm: exhaustion hands out exactly the free pages", n == s0.free_pages && s1.free_pages == 0);
    check("pmm: exhaustion never hands out reserved or out-of-range pages", bad == 0);
    check("pmm: exhausted allocator returns 0 and counts the failure", pmm_alloc_page() == 0 && s1.alloc_failures == s0.alloc_failures + 1);
    uint64_t freed_bad = 0;
    while (head) { uintptr_t next = *(volatile uint64_t *)head; if (pmm_free_page(head) != PMM_OK) freed_bad++; head = next; }
    pmm_get_stats(&s1);
    check("pmm: freeing everything restores the free count", freed_bad == 0 && s1.free_pages == s0.free_pages);
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
