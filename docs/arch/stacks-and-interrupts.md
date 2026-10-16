# Growing C stacks and CPU interrupts

RCC's conventional stack is selected by SB, with SP/FP offsets inside that bank.
RFL continues to use its heap activation records. C stack growth is an opt-in
calling convention; fixed-stack compilation remains the default for existing
programs, boot code, and interrupt handlers.

## Segmented C stacks

Compile normal C translation units with `rcc compile --segmented-stack` (also
available on `generate-asm`). Build the matching C runtime with
`make -C runtime SEGMENTED_STACK=1 BANK_SIZE=64000`. Rebuild all normal C
callers/callees together. RFL's existing C boundary uses the fixed convention;
mixing it with segmented C requires an explicit adapter.

Before making calls that can exhaust the root bank:

```c
#include <stack.h>

int main(void) {
    rcc_stack_context stack;
    if (rcc_stack_reserve(&stack, 16)) return 1;
    /* Normal C calls and recursion can now grow into sixteen more banks. */
    if (rcc_stack_destroy(&stack)) return 2;
    return 0;
}
```

Each context owns a pool. `rcc_stack_reserve` obtains an aligned pool from malloc;
`rcc_stack_init` instead accepts exclusively owned consecutive banks for an OS
allocator. Pool setup must fit the root stack, and the context must remain alive
until teardown. It can live in a root local or global object. Pool growth never
invokes malloc, takes a shared emergency stack, or accesses another context's
free list. Contexts used concurrently must own disjoint banks and metadata.
The kernel must arrange valid RAM and reserve the interrupt stack separately.

The backend checks capacity **before** writing the prologue. Its requirement
includes saved registers, final spill extent, locals, and the largest outgoing
argument area. Every frame still fits within one bank. At a transition the
runtime copies the incoming argument area, installs a return trampoline, and
continues the original prologue on a free bank. The trampoline restores the
previous SB/SP; the ordinary epilogue restores FP and saved registers.

Ten cells at the end of every participating bank are reserved:

| Offset from BANK_SIZE-10 | Contents |
| --- | --- |
| 0 | Next free segment bank |
| 1–2 | Previous SB and SP |
| 3–4 | Original RA and RAB |
| 5–6 | Context pointer offset and concrete bank |
| 7–9 | Reserved |

X2 carries the number of incoming stack argument cells. Handwritten assembly
calling a segmented function must supply it, including zero for register-only
calls. X3 and T6/T7 are scratch during the slow entry path. The ordinary seven-cell
frame header and A0–A3 parameter slots retain their layout. Naked assembly helpers
must preserve S0–S3, FP, GP, and all result words as required by the C ABI.

Escaping stack pointers carry their concrete bank instead of a tag interpreted
against the callee's SB. Aggregate results also return a two-cell pointer with
the actual bank; the caller copies the result before another call can reuse its
storage. All C objects must be rebuilt for this corrected aggregate-result ABI.

Segments are recycled in their context, keeping returned aggregate storage
intact until another growing call. Teardown frees a malloc-owned pool only after
all segments have unwound. Exhaustion, an unconfigured growing stack, or an
incoming argument/frame combination that cannot fit a bank prints `STACK!` and
halts with RV0=125 before frame writes. Oversized static frames are compile errors.

There is no setjmp/longjmp or unwinding API yet. A nonlocal jump across segments
must not be added without restoring the context's segment ownership too.
Unchecked bootstrap helpers, handwritten stack manipulation, and fixed code must
still fit their selected stack; the growing convention does not rewrite those.

## IRQ ABI version 2

The old IRQ contract overwrote RA/RAB and used RET. Version 2 preserves those
registers and has separate CPU return state. **Old IRQ binaries must be rebuilt.**
The device mailbox at MMIO 27–31 and the simulation clock are unchanged.

Entry occurs before the interrupted instruction. The lowest enabled pending
source is removed from the pending mask and latched as the active cause. The CPU
saves PC/PCB, the enable mask, and SB/SP/FP. BUSY inhibits further entry until
IRET. Other general registers, including RA/RAB and GP, are unchanged by entry.
Assembly entry code preserves all additional registers it will modify before
calling C. C calling conventions alone are not asynchronous interrupt wrappers.

If configured, entry selects an interrupt SB and sets SP/FP to its top. Setting
the top to zero leaves the interrupted stack selected. Interrupt stack banking
does not provide memory protection or a supervisor mode. Nested IRQs are disabled.

Writing nonzero to MMIO IRQ_BUSY acknowledges the active event but **does not**
clear BUSY, CAUSE, or any newly pending event. `IRET` restores CPU return state and
clears BUSY atomically, with no PC increment. An unacknowledged active event is
made pending again. The next instruction boundary may deliver another event.
Returning with RET leaves BUSY set and does not restore the interrupted PC.

## CPU instructions and control registers

| Instruction | Opcode | Encoding / effect |
| --- | --- | --- |
| IRET | 0x22 | No operands; atomic trap return |
| CSRR rd, csr | 0x23 | word1=destination register, word2=CSR number |
| CSRW rs, csr | 0x24 | word1=source register, word2=CSR number |

CSRR cannot target PC or PCB. Unknown CSRs, writes to read-only CSRs, trap-state
writes outside a handler, and IRET outside a handler are execution errors.

| CSR | Name | Access |
| --- | --- | --- |
| 0–1 | Saved PC / PCB | R/W during handler; reads return zero outside handler |
| 2 | Saved enable mask | R/W during handler |
| 3–4 | Timer period low/high | R/W, 32-bit guest-instruction interval |
| 5–6 | Interrupt stack bank/top | R/W; top zero disables switching |
| 7–9 | Saved SB / SP / FP | R/W during handler |
| 10 | Pending sources | Read mask; write-one-to-clear |
| 11 | IRQ ABI version | Read-only, value 2 |

A scheduler may change saved continuation, stack, and enable state before IRET,
while restoring the remaining task registers in assembly. `runtime/include/irq.h`
provides fixed-stack C helpers for CPU state, timer configuration, and stack setup.

IRQ_SW is bit 0; IRQ_TIMER is bit 1. The timer advances with executed guest
instructions, including handler work, independent of NEXT_TICK and host pacing.
Zero period disables it. Writing either period word restarts the countdown;
`irq_set_timer` masks IRQs while writing both words and clears stale timer events.
Repeated expirations coalesce into one pending bit. An event raised during a
handler survives acknowledgement of the earlier event of the same source.

The interpreter and JIT stop at the same timer boundaries. CPU system instructions
use the reference execution handler when reached from generated native code.
There is no extra timer work when disabled beyond the boundary check.

The rOS handler and application-return path use IRET. rOS remains a synchronous
software-call example; this change does not add a scheduler or complete its
unimplemented file syscalls. Preserve existing disks and build a new rOS image
to test the new ABI.

The Brainfuck CPU also implements IRQ ABI 2. Its interrupt machinery is compiled
out by default: `rlink --bf-irq` enables it, or set `ENABLE_IRQ=1` in standalone
macro output before expansion. The disabled variant contains no boundary polling
or timer accounting. The enabled variant counts guest instructions and keeps
interrupt state on the BF tape; RBF's host and JIT do not execute IRQ instructions
on its behalf. See [Brainfuck CPU configuration](../../src/ripple-asm/BF_HOST.md#code-memory-and-optional-interrupts).
