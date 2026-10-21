/* Real asynchronous IRQ/IRET integration probe, linked only into boot tests. */
.text
.globl arch_irq_probe
.type arch_irq_probe,@function
arch_irq_probe:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    cli
    fninit
    fld1
    mov %rsp, irq_registers+120(%rip)
    .set off,-128
    .rept 16
    movq $0x1728394a, off(%rsp)
    .set off,off+8
    .endr
    .set val,1
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    mov $val, %eax
    movd %eax, %xmm\n
    pshufd $0, %xmm\n, %xmm\n
    .set val,val+1
    .endr
    fxsave64 irq_fx_before(%rip)
    .set val,0x1234567800000001
    .irp reg,rax,rcx,rdx,rbx,rbp,rsi,rdi,r8,r9,r10,r11,r12,r13,r14,r15
    movabs $val, %\reg
    .set val,val+1
    .endr
    std
    sti
    hlt
    /* Snapshot before using any GPRs; another IRQ may safely arrive here. */
    .set off,0
    .irp reg,rax,rcx,rdx,rbx,rbp,rsi,rdi,r8,r9,r10,r11,r12,r13,r14,r15
    mov %\reg, irq_registers+off(%rip)
    .set off,off+8
    .endr
    mov %rsp, irq_registers+128(%rip)
    fxsave64 irq_fx_after(%rip)
    /* Check the entire red zone before PUSHF overwrites any of it. */
    movl $1, irq_redzone_ok(%rip)
    .set off,-128
    .rept 16
    cmpq $0x1728394a, off(%rsp)
    jne 1f
    .set off,off+8
    .endr
    jmp 2f
1:  movl $0, irq_redzone_ok(%rip)
2:  pushfq
    popq irq_registers+136(%rip)
    cli
    /* IRET must restore IF and DF; C must receive DF clear. */
    cld
    fninit
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
.size arch_irq_probe, .-arch_irq_probe
.bss
.balign 16
.globl irq_fx_before,irq_fx_after,irq_registers,irq_redzone_ok
irq_fx_before: .skip 512
irq_fx_after: .skip 512
irq_registers: .skip 144
irq_redzone_ok: .skip 4
.section .note.GNU-stack,"",@progbits
