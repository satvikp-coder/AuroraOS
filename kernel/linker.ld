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

SECTIONS
{
    . = 0x40080000;
    __image_start = .;

    __text_start = .;
    .text : { *(.text.boot) *(.text) *(.text.*) }
    . = ALIGN(4096);
    __text_end = .;

    __rodata_start = .;
    .rodata : { *(.rodata) *(.rodata.*) *(.user_blob) }
    . = ALIGN(4096);
    __rodata_end = .;

    __data_start = .;
    .data : { *(.data) *(.data.*) }
    .bss : {
        . = ALIGN(8);
        __bss_start = .;
        *(.bss) *(.bss.*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }
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
    }
    . = ALIGN(4096);
    __kernel_end = .;

    ASSERT((__kernel_end - __image_start) <= 0x100000, "kernel image exceeds the 1 MiB size declared in the arm64 Image header")

    /DISCARD/ : { *(.comment) *(.note.gnu.build-id) *(.eh_frame) *(.eh_frame_hdr) }
}
