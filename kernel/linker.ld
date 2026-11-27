/*
 * kernel/linker.ld — AuroraOS linker script (QEMU ARM64 virt)
 *
 * Every permission domain starts on a 4 KiB boundary so the MMU can map
 * text read-only/executable, rodata read-only/no-execute, and data, bss
 * and stack read-write/no-execute. A guard page sits below the stack and
 * is left unmapped to catch overflow.
 */
OUTPUT_FORMAT("elf64-littleaarch64", "elf64-littleaarch64", "elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

/* Match the runtime permission domains in the diagnostic ELF, too. */
PHDRS
{
    text   PT_LOAD FLAGS(5); /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4); /* PF_R */
    data   PT_LOAD FLAGS(6); /* PF_R | PF_W */
    stack  PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
    . = 0x40080000;
    __image_start = .;

    __text_start = .;
    .text : { *(.text.boot) *(.text) *(.text.*) } :text
    . = ALIGN(4096);
    __text_end = .;

    __rodata_start = .;
    .rodata : { *(.rodata) *(.rodata.*) *(.user_blob) *(.user_fp_blob) *(.user_mem_blob) } :rodata
    . = ALIGN(4096);
    __rodata_end = .;

    __data_start = .;
    .data : { *(.data) *(.data.*) } :data
    .bss : {
        . = ALIGN(8);
        __bss_start = .;
        *(.bss) *(.bss.*) *(COMMON)

        /* PMM bitmap arrays: sized for PMM_MAX_PAGES (1 GiB / 4 KiB = 262144 pages)
         * Each bitmap needs PMM_MAX_PAGES/64 = 4096 uint64_t words = 32 KiB.
         * At runtime, pmm_init() only uses the first bm_words entries based on actual RAM. */
        . = ALIGN(8);
        __pmm_used_bm_start = .;
        . += 4096 * 8;  /* PMM_MAX_PAGES/64 * 8 bytes */
        __pmm_used_bm_end = .;

        . = ALIGN(8);
        __pmm_rsv_bm_start = .;
        . += 4096 * 8;  /* PMM_MAX_PAGES/64 * 8 bytes */
        __pmm_rsv_bm_end = .;

        . = ALIGN(8);
        __bss_end = .;
    } :data
    . = ALIGN(4096);
    __data_end = .;

    .stack (NOLOAD) : {
        __stack_guard = .;
        . += 4096;
        __stack_bottom = .;
        . += 16384;
        __stack_top = .;
        /* Separate exception stack (SP_EL1) with its own unmapped guard page, so a fault caused by an
         * exhausted kernel stack can still save its frame and print diagnostics. */
        __exc_guard = .;
        . += 4096;
        __exc_stack_bottom = .;
        . += 8192;
        __exc_stack_top = .;
    } :data
    . = ALIGN(4096);
    __kernel_end = .;

    ASSERT((__kernel_end - __image_start) <= 0x100000, "kernel image exceeds the 1 MiB size declared in the arm64 Image header")

    /DISCARD/ : { *(.comment) *(.note.gnu.build-id) *(.eh_frame) *(.eh_frame_hdr) }
}
