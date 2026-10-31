OUTPUT_FORMAT(elf64-x86-64)
PHDRS
{
    limine_requests PT_LOAD;
    text PT_LOAD;
    rodata PT_LOAD;
    data PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;
    kernel_start = .;
    requests_start = .;
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests
    requests_end = .;
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    text_start = .;
    .text : {
        *(.text .text.*)
    } :text
    text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
    rodata_end = .;

    .note.gnu.build-id : {
        *(.note.gnu.build-id)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    data_start = .;
    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    data_end = .;
    kernel_end = .;
    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}
