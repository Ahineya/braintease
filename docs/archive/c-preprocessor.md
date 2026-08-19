> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

Stage 0: Preprocessor (rpp)

Goal: Turn .c + headers into a token stream / .i that the compiler proper consumes.

Driver flags
•	-E preprocess only → write .i
•	-I <dir> user include path (search order: dir of source → -I dirs → -isystem dirs)
•	-isystem <dir> system include path (suppresses some warnings)
•	-DNAME[=value], -UNAME, -include <file>
•	-M, -MM, -MF <file>, -MP dependency outputs (for build systems)
•	Builtins: __FILE__, __LINE__, __DATE__, __TIME__, __STDC__=1, __STDC_VERSION__=199901L

Features to implement (C99)
•	Object-like and function-like macros, variadics (...), # stringize, ## paste
•	Conditionals: #if/#elif/#else/#endif, #ifdef/#ifndef, defined()
•	Includes: #include "x.h" (user search) and #include <x.h> (system search)
•	#pragma once and classic include guards (no behavior beyond one-shot suppression)
•	#line, #error, #warning (emit diagnostics)
•	Comment handling (/*…*/ removed, //… to eol) before macro expansion
•	Trigraphs/UCNs: ignore trigraphs; support \uXXXX/\UXXXXXXXX in strings/chars lexically

Outputs
•	Either an in-memory token stream for the parser or a .i file (with -E).
•	Optional .d dependency file when requested.

Header placement policy
•	Public project headers → project/include/... (add with -I)
•	Private/internal headers → near sources or include/<pkg>/internal/...
•	Ripple system headers (libc, MMIO, intrinsics) → $RIPPLE_HOME/include (added via -isystem)

⸻

Sections model (.text / .rodata / .data / .bss) for Ripple

Why: even on VM, keeping ELF-like sections in the object model makes C semantics and the linker sane.

Compiler emission rules
•	.text: functions, jump tables for switch (if you generate them)
•	.rodata: string literals; static const objects with link-time known initializers; const file-scope objects; vtables; read-only tables
(Note: C’s const doesn’t imply “not addressable”; we still place in rodata unless volatile.)
•	.data: variables with non-zero initializers (int x=7; int a[3]={1,2,3};) and non-const aggregates
•	.bss: zero-initialized or tentative definitions (int x; static int buf[1024];)
•	.ctors/.dtors (optional later): arrays of constructor/destructor function pointers for __attribute__((constructor)).

Ripple placement (banks)
•	Reserve bank 0: MMIO (OUT=0, OUT_FLAG=1, etc.).
•	Give the linker a default script that packs:
•	.text into a code bank region (e.g., bank 2+; you already bank program blocks)
•	.rodata into a read-only convention bank (e.g., bank 1)
•	.data and .bss into a writable bank (e.g., bank 3)
•	Let users override with a linker script if they want different banking.

Startup (rcrt0) responsibilities
•	Zero .bss:

// Pseudocode using ISA
// r0 is zero, rA/rAB are call RA banks as usual
// R3..R7 are scratch here
LI   R3, __bss_start_bank
LI   R4, __bss_start_addr
LI   R5, __bss_end_bank
LI   R6, __bss_end_addr



bss_loop:
BEQ  R3, R5, bss_last_bank
bss_bank_fill:
STORE R0, R3, R4
ADDI  R4, R4, 1
BLT   R4, BANK_SIZE, bss_bank_fill
LI    R4, 0
ADDI  R3, R3, 1
JAL   R0, R0, bss_loop
bss_last_bank:
BLT   R4, R6, bss_last_fill
JAL   R0, R0, call_main
bss_last_fill:
STORE R0, R3, R4
ADDI  R4, R4, 1
JAL   R0, R0, bss_last_fill
call_main:
JAL   R0, R0, main
HALT

(Linker must export `__bss_*`, `BANK_SIZE`, etc.)
- **Rhe linker writes initial contents directly into the runtime bank → no copy needed.

**Linker symbols to export**

__text_start/__text_end
__rodata_start/__rodata_end
__data_start/__data_end
__bss_start/__bss_end
__stack_top

**How C maps to sections (quick cheat)**
- `const char msg[] = "hi";`            → `.rodata`
- `static const int T[3] = {1,2,3};`    → `.rodata` (local symbol)
- `int x = 5;`                          → `.data`
- `int x;` (global or static)           → `.bss`
- `static int y;` in a function         → `.bss` (function-local static)
- `char *p = msg;`                      → `.data` (relocation to `.rodata`)
- string literals                        → pooled in `.rodata` (dedup optional)

---

# Pipeline update (driver view)

rcc (driver)
├── rpp (preprocess)            : .c + headers → tokens / .i
├── front-end (parse/typecheck) : tokens → AST → IR
├── codegen                     : IR → Ripple .pobj
├── rasm (optional)             : .s → .pobj (if using an asm step)
└── rlink                       : .pobj → final macro/BF image

**Objects (`.pobj`)**
- Must carry **sections**, **symbols**, and **relocations** (banks + addresses).
- Each section has `(bank_hint, alignment, contents, reloc list)`.
- The linker script (builtin or user-supplied) decides banks/addresses and fixes relocs.

---

# Minimal examples

**Headers + sections**
```c
// include/api.h
#pragma once
extern const char hello[];   // declaration only
extern int counter;          // declaration

// src/data.c
#include "api.h"
const char hello[] = "Hello, Ripple!\n";   // → .rodata
int counter = 42;                           // → .data
static int cache[256];                      // → .bss

// src/main.c
#include "api.h"
int main(void) {
    // use hello/counter…
    return 0;
}

Build

rcc -target ripple -Iinclude -c src/data.c -o data.pobj
rcc -target ripple -Iinclude -c src/main.c -o main.pobj
rlink main.pobj data.pobj -o program.bfm

Startup guarantees (documented for users)
	•	All objects in .bss are zero on entry to main.
	•	All .data objects contain their initializers.
	•	.rodata is read-only by convention; the ISA doesn’t enforce it, but the toolchain won’t place stores into that bank in generated code.

⸻

That should plug the holes: headers exist in the preprocessor/driver part of the plan, and .rodata/.data/.bss are now explicit in codegen, linking, and runtime. If you want, I can whip up the tiny rcrt0 you can drop into your assembler to zero .bss and call main.