/* tests/programs/user.ld — links a loader test program as a compact, static ET_EXEC with three PT_LOAD segments (R+X text, R rodata, R+W data/bss),
 * each starting a new page of the user load window while the FILE stays contiguous (offset and vaddr agree modulo 4096, as the loader requires). */
ENTRY(_start)
PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}
SECTIONS
{
    . = 0x4000010000 + 0x100;
    .text   : { *(.text._start) *(.text .text.*) } :text
    . = ALIGN(4096) + (. & 4095);
    .rodata : { *(.rodata .rodata.* ) } :rodata
    . = ALIGN(4096) + (. & 4095);
    .data   : { BYTE(1); *(.data .data.*) } :data   /* never empty: the loader refuses a zero-size PT_LOAD */
    .bss    : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.ARM.attributes) *(.gnu.*) *(.interp) *(.dynamic) }
}
