ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(6);
}
SECTIONS {
    . = 0;
    .text : ALIGN(4096) { KEEP(*(.text.entry)) *(.text .text.*) } :text
    .rodata : ALIGN(4096) { *(.rodata .rodata.*) } :rodata
    .rela.dyn : ALIGN(8) { *(.rela.dyn) } :rodata
    . = ALIGN(4096);
    kernel_readonly_end = .;
    .data : ALIGN(4096) { *(.data .data.*) *(.got .got.*) } :data
    .dynamic : ALIGN(8) { *(.dynamic) } :data :dynamic
    .bss : ALIGN(4096) { *(.bss .bss.*) *(COMMON) } :data
    . = ALIGN(4096);
    kernel_end = .;
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
