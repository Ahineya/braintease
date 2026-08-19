# Ripple C Toolchain

Architecture (registers, ISA, C ABI, banks): [`docs/arch/`](docs/arch/README.md).

C99 source is compiled to Ripple assembly, assembled, linked with startup code and a runtime library, then executed. The **primary target is the Ripple VM** (`rvm`). A Brainfuck backend still exists for programs that do not need MMIO/display.

```
C  →  rcc  →  .asm  →  rasm  →  .pobj  →  rlink (+ crt0 + libruntime)  →  .bin  →  rvm
                                                                      ↘  .bfm  →  bfm expand  →  bf
```

## Building the tools

From the repo root:

```bash
cargo build --release                          # rcc, rcpp, rvm, rct
(cd src/ripple-asm && cargo build --release)   # rasm, rlink
```

`npm run build:native` also builds the assembler, interpreter, macro expander, `rbt`, and the workspace crates.

Binaries:

| Tool | Path |
|------|------|
| `rcc` | `target/release/rcc` |
| `rcpp` | `target/release/rcpp` |
| `rvm` | `target/release/rvm` |
| `rct` | `target/release/rct` (also `./rct` from repo root) |
| `rasm` | `src/ripple-asm/target/release/rasm` |
| `rlink` | `src/ripple-asm/target/release/rlink` |

Prefer the local `target/` binaries over copies in `/usr/local/bin`.

**Bank size must match** across runtime, `rcc`, `rasm`, and `rlink`. `rct` and `runtime/Makefile` default to **64000**. CLI defaults differ (`rcc` 4096, `rasm`/`rlink` 16) — always pass `--bank-size` explicitly in a manual build.

## Running examples and tests (`rct`)

`rct` is the usual way to compile and run C under `c-test/`. It rebuilds the runtime, preprocesses, compiles, assembles, links, and runs.

```bash
# Tests (expected-output suite)
./rct                          # all tests, RVM backend
./rct test_hello               # one test, by stem
./rct -c core/typedef          # one category
./rct --backend bf test_hello  # Brainfuck backend

# Examples (no expected output; just compile and run)
./rct run sierpinski           # stdout
./rct run text40_galaga        # TEXT40 terminal display
./rct run rgb565_plasma --visual   # RGB565 window
./rct run text40_galaga --frequency 2MHz
```

`rct run` finds files under `c-test/` by stem (`text40_galaga` → `c-test/examples/text40_galaga.c`). Artifacts land in `c-test/build/`.

Display modes:

- **stdout / TTY** — no extra flags
- **TEXT40** (40×25 color cells in the terminal) — do **not** pass `--visual`
- **RGB565** (pixel window) — pass `--visual`

MMIO and display examples need the RVM backend.

## Toolchain components

### rcc — C99 compiler

```bash
rcc compile source.c -o output.asm -I runtime/include --bank-size 64000
```

Useful flags: `-I` include dirs, `-D` macros, `--no-preprocess`, `--save-ir`, `--print-ir`, `--emit-ir`, `--debug 0..3`, `--trace`.

`rcc` does not emit startup code; programs are linked with `crt0`.

### rcpp — preprocessor

Standalone preprocessor (what `rct` uses before `rcc --no-preprocess`):

```bash
rcpp source.c -o source.pp.c -I runtime/include
```

`rcc compile` can preprocess itself when you pass `-I` and omit `--no-preprocess`.

### rasm — assembler

```bash
rasm assemble source.asm -o output.pobj --bank-size 64000 --max-immediate 65535
```

Default output is a JSON object file (`.pobj`). Also: `rasm disassemble program.bin -o program.asm`, `rasm check`, `rasm --reference`.

### rlink — linker

```bash
# RVM binary (default format)
rlink crt0.pobj libruntime.par main.pobj -f binary --bank-size 64000 -o program.bin

# Brainfuck macros
rlink crt0.pobj libruntime.par main.pobj -f macro --standalone --bank-size 64000 -o program.bfm

# Library archive
rlink file1.pobj file2.pobj -f archive -o library.par
```

Formats: `binary` (RLINK image for `rvm`), `macro` (Brainfuck macros), `text` (listing), `archive` (`.par`). `--standalone` / `--debug` apply to macro output.

Linking order: `crt0.pobj` first (contains `_start`), then libraries, then the program.

### rvm — Ripple VM

```bash
rvm program.bin
rvm program.bin --visual          # RGB565 window
rvm program.bin -t                # TUI debugger
rvm program.bin --frequency 1MHz
rvm program.bin --disk path.img   # storage image (default: ~/.RippleVM/disk.img)
```

See [`rvm/mmio.md`](rvm/mmio.md) and [`docs/arch/mmio.md`](docs/arch/mmio.md) for TEXT40, RGB565, keyboard, RNG, storage, and IRQ MMIO.

### Runtime library

`runtime/` is built with `make` (or `./rct build-runtime`). That produces:

- `libruntime.par` — archive of runtime objects (not including crt0)
- `crt0.pobj` — startup; built separately via `make crt0.pobj`

Headers live in `runtime/include/`:

| Header | Contents |
|--------|----------|
| `stdio.h` | `putchar`, `puts`, `getchar`, `printf` |
| `stdlib.h` | `malloc` / `free` / `calloc` / `realloc`, `rand` / `srand` |
| `string.h` | `strlen`, `strcpy`, `memcpy`, `memset`, … |
| `mmio.h` | TTY, RNG, TEXT40, keyboard, storage, IRQ |
| `graphics.h` | RGB565 drawing |
| `mmio_constants.h` | MMIO addresses, display modes, palette |

Sources currently archived: `__char_patch.c`, `putchar.c`, `puts.c`, `getchar.c`, `mmio.c`, `rand.c`, `graphics.c`, `string.c`, `malloc.c`.

```bash
cd runtime
make clean
make all BANK_SIZE=64000
make crt0.pobj BANK_SIZE=64000
```

## Manual compile of a C program

Same pipeline `rct` uses, with `rcc` doing the preprocess step:

```bash
BANK=64000
INC=runtime/include

# Runtime (once, same BANK)
(cd runtime && make clean && make all BANK_SIZE=$BANK && make crt0.pobj BANK_SIZE=$BANK)

rcc compile program.c -o program.asm -I $INC --bank-size $BANK
rasm assemble program.asm -o program.pobj --bank-size $BANK --max-immediate 65535
rlink runtime/crt0.pobj runtime/libruntime.par program.pobj \
  -f binary --bank-size $BANK -o program.bin
rvm program.bin
```

Brainfuck instead of RVM:

```bash
rlink runtime/crt0.pobj runtime/libruntime.par program.pobj \
  -f macro --standalone --bank-size $BANK -o program.bfm
bfm expand program.bfm -o program.bf
bf program.bf --cell-size 16 --tape-size 150000000
```

## Building a multi-file program

**main.c:**
```c
#include <stdio.h>

int add(int a, int b);

int main() {
    int result = add(5, 3);
    putchar('0' + result);
    putchar('\n');
    return 0;
}
```

**math.c:**
```c
int add(int a, int b) {
    return a + b;
}
```

```bash
BANK=64000
INC=runtime/include

rcc compile main.c -o main.asm -I $INC --bank-size $BANK
rcc compile math.c -o math.asm -I $INC --bank-size $BANK
rasm assemble main.asm -o main.pobj --bank-size $BANK --max-immediate 65535
rasm assemble math.asm -o math.pobj --bank-size $BANK --max-immediate 65535
rlink runtime/crt0.pobj runtime/libruntime.par main.pobj math.pobj \
  -f binary --bank-size $BANK -o program.bin
rvm program.bin
```

## Example Makefile

```makefile
RCC = ../target/release/rcc
RASM = ../src/ripple-asm/target/release/rasm
RLINK = ../src/ripple-asm/target/release/rlink
RVM = ../target/release/rvm

BANK_SIZE = 64000
MAX_IMMEDIATE = 65535
INCLUDE = ../runtime/include

RUNTIME_DIR = ../runtime
CRT0 = $(RUNTIME_DIR)/crt0.pobj
RUNTIME_LIB = $(RUNTIME_DIR)/libruntime.par

C_SOURCES = main.c math.c
ASM_FILES = $(C_SOURCES:.c=.asm)
OBJ_FILES = $(C_SOURCES:.c=.pobj)
PROGRAM = myprogram.bin

$(PROGRAM): $(OBJ_FILES) $(CRT0) $(RUNTIME_LIB)
	$(RLINK) $(CRT0) $(RUNTIME_LIB) $(OBJ_FILES) -f binary --bank-size $(BANK_SIZE) -o $(PROGRAM)

%.asm: %.c
	$(RCC) compile $< -o $@ -I$(INCLUDE) --bank-size $(BANK_SIZE)

%.pobj: %.asm
	$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMMEDIATE)

run: $(PROGRAM)
	$(RVM) $(PROGRAM)

clean:
	rm -f $(ASM_FILES) $(OBJ_FILES) $(PROGRAM)

.PHONY: run clean
```

## Creating a library

```bash
BANK=64000
rcc compile mylib.c -o mylib.asm -I runtime/include --bank-size $BANK
rasm assemble mylib.asm -o mylib.pobj --bank-size $BANK --max-immediate 65535
rlink mylib.pobj -f archive -o libmylib.par

rcc compile main.c -o main.asm -I runtime/include --bank-size $BANK
rasm assemble main.asm -o main.pobj --bank-size $BANK --max-immediate 65535
rlink runtime/crt0.pobj runtime/libruntime.par libmylib.par main.pobj \
  -f binary --bank-size $BANK -o program.bin
```

Inspect an archive:

```bash
cat libruntime.par | jq '.objects[].name'
```

## Notes

1. **Labels**: the compiler prefixes labels with function names (`main_L1`, `add_L2`) so multiple translation units can link.

2. **crt0** (`runtime/crt0.asm`) sets up the C ABI and calls `main`. Current startup:

   ```asm
   _start:
       LI SB, 2        ; stack bank (SB/R28)
       LI SP, 1        ; stack pointer (SP/R29), grows upward
       LI FP, 1        ; frame pointer (FP/R30)
       LI GP, 1        ; global pointer (GP/R31)
       CALL _init_globals
       CALL main
       HALT
   ```

3. **Headers**: include `stdio.h` / `mmio.h` / etc. from `runtime/include` rather than hand-declaring `putchar`.

4. **Program size**: the linked image may occupy several IMEM banks. `rlink` places each function in one bank and NOP-pads to the next bank boundary when the remainder is too small. A **single function** larger than `--bank-size` instructions will not link. `rvm` loads multi-bank binaries; it does not reject images longer than one bank.

## Troubleshooting

### Duplicate label
Same function defined in more than one file, or mixing objects built with an old compiler that did not prefix labels.

### Unresolved reference
Missing `libruntime.par` / `crt0.pobj`, or a declaration that does not match a definition. `rasm` reports unresolved symbols per object; `rlink` fails if they are still unresolved after linking.

### Wrong bank size
Rebuild **runtime and program** with the same `--bank-size`. Mixing 4096 runtime objects with a 64000 program (or the reverse) produces bad binaries.

### Display does nothing / garbled terminal
TEXT40 takes over the terminal; RGB565 needs `rvm --visual` (or `./rct run … --visual`). Do not use `--visual` for TEXT40.

### Runtime errors
Check `crt0.asm` stack/global setup, then run `rvm -t program.bin` or `./rct debug test_name`.

## Quick reference

```bash
# Examples
./rct run sierpinski
./rct run text40_galaga
./rct run rgb565_plasma --visual

# Tests
./rct
./rct test_hello
./rct --backend bf test_hello

# Manual RVM pipeline
rcc compile program.c -o program.asm -I runtime/include --bank-size 64000
rasm assemble program.asm -o program.pobj --bank-size 64000 --max-immediate 65535
rlink runtime/crt0.pobj runtime/libruntime.par program.pobj \
  -f binary --bank-size 64000 -o program.bin
rvm program.bin

# Library
rlink file1.pobj file2.pobj -f archive -o mylib.par

# Assemble-only helper (not the C compiler)
rbt program.asm --run
```
