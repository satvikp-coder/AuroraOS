/*
 * kernel/linker.ld — AuroraOS linker script
 *
 * Places the boot entry at 0x40080000 (the QEMU ARM64 virt -kernel load
 * address), followed by .text, .rodata, .data, and .bss.
 */

OUTPUT_FORMAT("elf64-littleaarch64", "elf64-littleaarch64",
              "elf64-littleaarch64")
OUTPUT_ARCH(aarch64)

ENTRY(_start)

SECTIONS
{
    /* . = 0x40080000; */   /* QEMU virt -kernel load address */

    . = 0x40080000;

    .text.boot : {
        *(.text.boot)
    }

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.gnu.build-id)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
