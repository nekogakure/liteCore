OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    /* User programs start at 0x400000 (4MB) */
    . = 0x400000;

    /* Read-only sections */
    .text : ALIGN(0x1000) {
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    /* Read-write sections */
    .data : ALIGN(0x1000) {
        *(.data .data.*)
        *(.got .got.plt)
    }

    /* Constructor/Destructor arrays */
    .init_array : ALIGN(8) {
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        KEEP(*(.ctors))
        __init_array_end = .;
    }

    .fini_array : ALIGN(8) {
        __fini_array_start = .;
        KEEP(*(.fini_array .fini_array.*))
        KEEP(*(.dtors))
        __fini_array_end = .;
    }

    .bss : ALIGN(0x1000) {
        *(COMMON)
        *(.bss .bss.*)
    }

    /* Discard unnecessary sections */
    /DISCARD/ : {
        *(.note.gnu.build-id)
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
