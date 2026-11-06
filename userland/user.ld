OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD      FLAGS(5); /* R + X */
    rodata PT_LOAD      FLAGS(4); /* R */
    data   PT_LOAD      FLAGS(6); /* R + W */
    stack  PT_GNU_STACK FLAGS(6); /* R + W, non-executable */
}

SECTIONS
{
    . = 0x00400000;
    .text : ALIGN(0x1000)
    {
        KEEP(*(.text.start))
        *(.text .text.*)
    } :text

    .rodata : ALIGN(0x1000)
    {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } :rodata

    . = ALIGN(0x1000);
    .data :
    {
        *(.data .data.*)
        *(.got .got.*)
    } :data

    .bss (NOLOAD) : ALIGN(0x1000)
    {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
    }
}
