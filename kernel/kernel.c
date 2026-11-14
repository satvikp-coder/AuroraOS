/*
 * kernel/kernel.c — AuroraOS kernel entry (C)
 *
 * Boot sequence: console -> exception vectors -> validated device-tree memory map ->
 * physical page allocator -> page tables + MMU -> self-tests.
 * Target: QEMU ARM64 "virt" (PL011 UART at 0x09000000). No Linux/Android code.
 *
 * The physical allocator is only initialised from a VALIDATED device tree. If none is
 * found the kernel stops with a diagnostic instead of guessing the RAM size. A fixed
 * 256 MiB QEMU layout exists only in builds that explicitly set AURORA_FIXED_RAM=1
 * (see build.sh); such a kernel prints a loud DEV MODE banner and must never be
 * used on hardware.
 */
#include <arch.h>
#include <blk.h>
#include <fs.h>
#include <console.h>
#include <gfx.h>
#include <gic.h>
#include <irq.h>
#include <irqtest.h>
#include <launch.h>
#include <settings.h>
#include <proctest.h>
#include <timer.h>
#include <mm.h>
#include <selftest.h>

#define QEMU_VIRT_RAM_BASE 0x40000000UL   /* used only by the explicit AURORA_FIXED_RAM dev mode */

/* Set by start.S just after BSS is cleared: the exception level and SPSel the CPU was entered with. */
uint64_t boot_entry_el, boot_entry_spsel;
static struct fdt_info boot_info;
static uint64_t boot_dtb;
uint64_t boot_dtb_address(void) { return boot_dtb; }
const struct fdt_info *boot_fdt_info(void) { return &boot_info; }

static void halt_forever(void) { for (;;) __asm__ volatile ("msr daifset, #0xf\n wfi"); }

static void fatal(const char *msg)
{
    console_puts("FATAL: "); console_puts(msg); console_puts("\nKernel halted; memory manager NOT initialised.\n");
    halt_forever();
}

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
    console_puts("[info] boot entry: EL"); console_dec(boot_entry_el); console_puts(", SPSel="); console_dec(boot_entry_spsel);
#ifdef AURORA_TEST_ENTRY
    console_puts(" (CONTROLLED TEST ENTRY MODE "); console_dec(AURORA_TEST_ENTRY); console_puts(")");
#endif
    console_puts("\n");
    console_puts("Exception vectors installed at EL"); console_dec(read_current_el()); console_puts("\n");

    /* 1. Memory map from a validated device tree (x0 is the Linux arm64 boot-protocol DTB pointer). */
    int rc = dtb ? fdt_parse((const void *)(uintptr_t)dtb, &boot_info) : FDT_ERR_NULL;
    if (rc == FDT_OK) {
        console_puts("Device tree at "); console_hex(dtb); console_puts(": RAM base "); console_hex(boot_info.ram_base);
        console_puts(" size "); console_hex(boot_info.ram_size); console_puts(", reserved regions: ");
        console_dec((uint64_t)boot_info.nreserved); console_puts("\n");
    } else {
#ifdef AURORA_FIXED_RAM
        console_puts("*** DEV MODE (AURORA_FIXED_RAM): no valid device tree ("); console_puts(fdt_strerror(rc));
        console_puts("); assuming 256 MiB at 0x40000000. NEVER use on real hardware. ***\n");
        boot_info.valid = 0; boot_info.nreserved = 0; boot_info.ram_base = QEMU_VIRT_RAM_BASE; boot_info.ram_size = 0x10000000UL;
        dtb = 0;
#else
        console_puts("FATAL: invalid or missing device tree: "); console_puts(fdt_strerror(rc)); console_puts("\n");
        fatal("refusing to guess the memory map");
#endif
    }

    boot_dtb = dtb;

    /* 2. Physical page allocator and reservations. */
    if (pmm_init(boot_info.ram_base, boot_info.ram_size) < 0) fatal("pmm_init rejected the RAM description");
    if ((uintptr_t)__kernel_end > pmm_ram_end() || (uintptr_t)__image_start < pmm_ram_base()) fatal("kernel image is outside RAM");
    int bad = 0;
    if ((uintptr_t)__image_start > pmm_ram_base()) bad |= pmm_reserve(pmm_ram_base(), (uintptr_t)__image_start); /* boot region */
    bad |= pmm_reserve((uintptr_t)__image_start, (uintptr_t)__kernel_end);                                          /* image, bss, stack */
    if (dtb) bad |= pmm_reserve_range(dtb, boot_info.totalsize);                                                    /* the blob itself */
    for (int i = 0; i < boot_info.nreserved; i++) {
        bad |= pmm_reserve_range(boot_info.reserved[i].addr, boot_info.reserved[i].size);
        if (boot_info.reserved[i].nomap && vmm_add_nomap(boot_info.reserved[i].addr, boot_info.reserved[i].size)) bad = 1;
    }
    if (bad) fatal("could not apply memory reservations");
    /* 2b. Optional display (QEMU ramfb): reserve its framebuffer now, while contiguous pages are still free. No display is a normal boot. */
    if (dtb) (void)gfx_reserve((const void *)(uintptr_t)dtb);
    struct pmm_stats st; pmm_get_stats(&st);
    console_puts("Physical memory manager ready\n");
    print_range("RAM          ", pmm_ram_base(), pmm_ram_end());
    print_range("kernel image ", (uintptr_t)__image_start, (uintptr_t)__kernel_end);
    console_puts("  pages total/reserved/free: "); console_dec(st.total_pages); console_puts("/");
    console_dec(st.reserved_pages); console_puts("/"); console_dec(st.free_pages); console_puts("\n");
    console_puts("  physical address width: "); console_dec((uint64_t)vmm_pa_bits()); console_puts(" bits\n");

    selftest_pmm();

    /* 3. Page tables and MMU. */
    int vr = vmm_init();
    if (vr < 0) { console_puts("vmm_init error "); console_dec((uint64_t)(-vr)); console_puts("\n"); fatal("page-table setup failed"); }
    console_puts("Enabling MMU...\n");
    vmm_enable();
    console_puts("MMU enabled (39-bit VA, 4 KiB pages, W^X)\n");

    selftest_mmu();
    int failures = selftest_summary();
    if (failures) { console_puts("MEMORY MANAGEMENT SELFTEST FAILED\n"); halt_forever(); }
    console_puts("Memory management OK\n");

    /* 3b. Native graphics: configure the display, then draw the diagnostic pattern ourselves. Failure leaves the UART boot untouched. */
    if (dtb && gfx_init() == GFX_OK) {
        if (gfx_draw_test_pattern(gfx_get_surface()) == GFX_OK) { gfx_present(0); console_puts("gfx: test pattern drawn\n"); }
        else console_puts("gfx: test pattern FAILED\n");
    }

    /* 4. Interrupt controller, timer, and their tests. The platform description must come from the device tree. */
    if (dtb) {
        struct fdt_platform plat;
        int pr = fdt_platform_parse((const void *)(uintptr_t)dtb, &plat);
        if (pr || plat.gic_version != 2 || !plat.have_timer) fatal("device tree does not describe a supported interrupt controller (GICv2) and ARM timer");
        int gr = gicv2_init(&plat);
        if (gr) { console_puts("gicv2_init error "); console_dec((uint64_t)(-gr)); console_puts("\n"); fatal("interrupt controller setup failed"); }
        int tr = timer_init(&plat);
        if (tr) { console_puts("timer_init error "); console_dec((uint64_t)(-tr)); console_puts("\n"); fatal("timer setup failed"); }
        console_puts("GICv2 at "); console_hex(plat.gicd_base); console_puts("/"); console_hex(plat.gicc_base);
        console_puts(", timer frequency "); console_dec(timer_freq()); console_puts(" Hz, interrupt ID "); console_dec(timer_intid()); console_puts("\n");
        irqtest_run(&plat);
        if (irqtest_summary()) { console_puts("INTERRUPT SELFTEST FAILED\n"); halt_forever(); }
        console_puts("Interrupts and timer OK\n");

        /* 4b. Storage: a virtio block device is optional. No disk is a normal boot; a disk that is present but unusable is reported. */
        int br = blk_init((const void *)(uintptr_t)dtb);
        if (br == BLK_OK) console_puts("Block device ready\n");
        else if (br == BLK_ENODEV) console_puts("[blk] no block device attached\n");
        else { console_puts("[blk] block device present but unusable: "); console_puts(blk_strerror(br)); console_puts("\n"); }
        blktest_run();
        if (blktest_summary()) { console_puts("BLOCK SELFTEST FAILED\n"); halt_forever(); }
        fstest_run();
        if (fstest_summary()) { console_puts("FILESYSTEM SELFTEST FAILED\n"); halt_forever(); }

        /* 5. Userspace: real EL0 processes, syscalls, scheduler, isolation. */
        proctest_run();
        if (proctest_summary()) { console_puts("PROCESS SELFTEST FAILED\n"); halt_forever(); }
        console_puts("Userspace OK\n");
        fsproc_run();
        if (fsproc_summary()) { console_puts("FILE ACCESS SELFTEST FAILED\n"); halt_forever(); }
        loadtest_run();
        if (loadtest_summary()) { console_puts("LOADER SELFTEST FAILED\n"); halt_forever(); }
        settingstest_run();
        if (settingstest_summary()) { console_puts("SETTINGS SELFTEST FAILED\n"); halt_forever(); }
        init_run();
    } else {
        console_puts("Interrupt tests skipped: no device tree (dev mode)\n");
    }

    selftest_fault();   /* no-ops unless this is a deliberate fault-test build */
    irqtest_fault();
    proctest_fault();
    console_puts("Boot complete, idling.\n");
    halt_forever();
}
