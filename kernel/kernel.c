/*
 * kernel/kernel.c — AuroraOS kernel entry (C)
 *
 * Boot sequence: console -> exception vectors -> device-tree memory map ->
 * physical page allocator -> page tables + MMU -> self-tests.
 * Target: QEMU ARM64 "virt" (PL011 UART at 0x09000000). No Linux/Android code.
 */
#include <arch.h>
#include <console.h>
#include <mm.h>
#include <selftest.h>

/* Fallback if QEMU does not hand us a usable device tree (should not happen on virt). */
#define FALLBACK_RAM_BASE 0x40000000UL
#define FALLBACK_RAM_SIZE 0x10000000UL

static void halt_forever(void) { for (;;) __asm__ volatile ("wfi"); }

static void print_range(const char *name, uintptr_t s, uintptr_t e)
{
    console_puts("  "); console_puts(name); console_puts(": "); console_hex(s); console_puts(" - "); console_hex(e); console_puts("\n");
}

void kernel_main(uint64_t dtb)
{
    console_puts("AuroraOS booting...\n");
    console_puts("ARM64 initialized\n");
    console_puts("Serial console ready\n");
    console_puts("Welcome to AuroraOS\n");

    exceptions_init();
    console_puts("Exception vectors installed at EL"); console_dec(read_current_el()); console_puts("\n");

    /* 1. Memory map from the device tree. */
    struct fdt_info fdt;
    uint64_t ram_base = FALLBACK_RAM_BASE, ram_size = FALLBACK_RAM_SIZE;
    /* Linux-style boot passes the DTB in x0; QEMU loads ELF kernels with the DTB at the start of RAM and x0=0. Probe both; fdt_parse validates the magic and every offset. */
    uint64_t cand[2] = { dtb, FALLBACK_RAM_BASE };
    int rc = -1;
    for (int i = 0; i < 2 && rc != 0; i++) {
        rc = cand[i] ? fdt_parse((const void *)(uintptr_t)cand[i], &fdt) : -1;
        if (rc == 0) dtb = cand[i];
    }
    if (rc == 0) {
        ram_base = fdt.ram_base; ram_size = fdt.ram_size;
        console_puts("Device tree at "); console_hex(dtb); console_puts(": RAM base "); console_hex(ram_base);
        console_puts(" size "); console_hex(ram_size); console_puts("\n");
    } else {
        console_puts("WARNING: no valid device tree (code "); console_dec((uint64_t)(-rc));
        console_puts("), using QEMU virt defaults\n");
        fdt.valid = 0; fdt.nreserved = 0;
    }

    /* 2. Physical page allocator and reservations. */
    if (pmm_init(ram_base, ram_size) < 0) { console_puts("PANIC: pmm_init failed\n"); halt_forever(); }
    int bad = 0;
    if ((uintptr_t)__image_start > pmm_ram_base()) bad |= pmm_reserve(pmm_ram_base(), (uintptr_t)__image_start); /* boot region incl. DTB */
    bad |= pmm_reserve((uintptr_t)__image_start, (uintptr_t)__kernel_end);                                          /* image, bss, stack */
    if (fdt.valid) {
        bad |= pmm_reserve((uintptr_t)dtb, (uintptr_t)dtb + fdt.totalsize);
        for (int i = 0; i < fdt.nreserved; i++) bad |= pmm_reserve(fdt.reserved[i].addr, fdt.reserved[i].addr + fdt.reserved[i].size);
    }
    if (bad) { console_puts("PANIC: reservation failed\n"); halt_forever(); }
    struct pmm_stats st; pmm_get_stats(&st);
    console_puts("Physical memory manager ready\n");
    print_range("RAM          ", pmm_ram_base(), pmm_ram_end());
    print_range("kernel image ", (uintptr_t)__image_start, (uintptr_t)__kernel_end);
    console_puts("  pages total/reserved/free: "); console_dec(st.total_pages); console_puts("/");
    console_dec(st.reserved_pages); console_puts("/"); console_dec(st.free_pages); console_puts("\n");

    selftest_pmm();

    /* 3. Page tables and MMU. */
    if (vmm_init() < 0) { console_puts("PANIC: vmm_init failed\n"); halt_forever(); }
    console_puts("Enabling MMU...\n");
    vmm_enable();
    console_puts("MMU enabled (39-bit VA, 4 KiB pages, W^X)\n");

    selftest_mmu();
    int failures = selftest_summary();
    if (failures) { console_puts("MEMORY MANAGEMENT SELFTEST FAILED\n"); halt_forever(); }
    console_puts("Memory management OK\n");

    selftest_fault();   /* no-op unless this is a deliberate fault-test build */
    console_puts("Boot complete, idling.\n");
    halt_forever();
}
