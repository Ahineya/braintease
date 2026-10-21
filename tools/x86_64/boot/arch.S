.section .text.entry,"ax",@progbits
.globl _start
.type _start,@function
_start:
    cli
    cld
    lea kernel_stack_top(%rip), %rsp
    xor %ebp, %ebp
    call kernel_main
    ud2
.size _start, .-_start

.text
.globl arch_load_gdt
.type arch_load_gdt,@function
arch_load_gdt:
    lgdt (%rdi)
    pushq $8
    lea 1f(%rip), %rax
    push %rax
    lretq
1:
    mov $16, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    mov $24, %ax
    ltr %ax
    ret
.size arch_load_gdt, .-arch_load_gdt

.globl exception_common
.type exception_common,@function
exception_common:
    cld
    mov %rsp, %rdi
    and $-16, %rsp
    call exception_fatal
    ud2
.size exception_common, .-exception_common

/* Every device/APIC gate uses IST2, protecting the interrupted SysV red zone. IF is
 * clear throughout this handler. No C16 code, allocation or switching here.
 * Baseline x86_64 code uses x87/SSE2; AVX/XSAVE are not enabled by this port. */
.globl irq_common
.type irq_common,@function
irq_common:
    cld
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, %rbx
    sub $512, %rsp
    and $-16, %rsp
    fxsave64 (%rsp)
#ifdef BOOT_TEST
    /* Make state restoration observable even though the C ISR uses no SIMD. */
    fninit
    ldmxcsr irq_test_mxcsr(%rip)
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    pxor %xmm\n, %xmm\n
    .endr
#endif
    lea 120(%rbx), %rdi
    call irq_dispatch
    fxrstor64 (%rsp)
    mov %rbx, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax
    add $16, %rsp
    iretq
.size irq_common, .-irq_common

#ifdef BOOT_TEST
.section .rodata
.balign 4
irq_test_mxcsr: .long 0x3f80
#endif

.section .bss.stack,"aw",@nobits
.balign 4096
.globl kernel_stack_guard
kernel_stack_guard:
    .skip 1048576
.globl kernel_stack_bottom
kernel_stack_bottom:
    .skip 1048576
.globl kernel_stack_top
kernel_stack_top:
.section .note.GNU-stack,"",@progbits
