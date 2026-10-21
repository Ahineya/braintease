/* Native linker oracle, not C16 guest data. Checks the loader's relocated
 * native pointer, page-aligned data, and zero-initialized native BSS. */
.text
.globl native_pointer_probe
native_pointer_probe:
    movq pointer(%rip), %rax
    leaq value(%rip), %rcx
    cmpq %rcx, %rax
    jne .Lbad
    movq (%rax), %rax
    cmpq $123, %rax
    jne .Lbad
    movq untouched(%rip), %rax
    testq %rax, %rax
    jne .Lbad
    leaq pointer(%rip), %rax
    andq $4095, %rax
    jne .Lbad
    movl $0, %eax
    ret
.Lbad:
    movl $1, %eax
    ret
.data
.balign 4096
pointer:
    .quad value
value:
    .quad 123
.bss
.balign 64
untouched:
    .zero 8
