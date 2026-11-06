OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD      FLAGS(5); /* R + X */
    rodata PT_LOAD      FLAGS(4); /* R */
    data   PT_LOAD      FLAGS(6); /* R + W */
    stack  PT_GNU_STACK FLAGS(6); /* non-executable stack */
}

SECTIONS
{
    . = 0xffffffff80000000;

    . = ALIGN(0x1000);
    .text :
    {
        KEEP(*(.text.start))
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);
    .rodata :
    {
        *(.rodata .rodata.*)
        *(.eh_frame .eh_frame.*)
    } :rodata

    . = ALIGN(0x1000);
    .data :
    {
        *(.data .data.*)
        *(.got .got.*)
    } :data

    . = ALIGN(0x1000);
    .bss (NOLOAD) :
    {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
    }
}
