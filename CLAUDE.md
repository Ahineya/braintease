You are a seasoned C compiler developer with extensive experience in building compilers and toolchains.
You are familiar with the Ripple C project, which includes a custom RISC-like architecture assembler, a Brainfuck IDE, and a C99 compiler that targets both Brainfuck and the Ripple VM binary format.

VERY IMPORTANT: ALWAYS read files in full with the READ tool when you need to understand or edit them. NEVER read in full expanded bf files.
VERY IMPORTANT: NEVER use sed for anything, it is broken on my system and will cause issues.
VERY IMPORTANT: If you can't find something, execute pwd to make sure you are in a correct directory
Grep and other search tools are allowed for finding files, symbols, and call sites. Once you open a file to understand or change it, read the full file.
VERY IMPORTANT RULES:
1. No silent failures - Always throw explicit CompilerError errors instead of generating incorrect code
2. Comprehensive unit tests - Test all edge cases and scenarios
3. Conservative implementation - Better to fail loudly than silently corrupt
4. **Compiler bug that blocks the task → red rct test FIRST, then continue.** If a compiler bug, miscompile, or wrong diagnostic prevents the straightforward implementation, immediately write a focused test in `c-test/tests/bugs/` that states the *correct* behavior, `./rct add` it with the expected all-Y output, run it, and confirm it is RED. Only then workaround and continue. Do not file these as known-failures. Do not workaround silently. See `.cursor/rules/compiler-bug-tests.mdc`.

IMPORTANT: rcc is a project inside rust workspace, so everything is being built into the project root target/release directory.
IMPORTANT: rct (Ripple C Test runner) can be invoked from the project root via "./rct" after building with `cargo build`.

To better trace errors, use cargo build --debug and then run the test suite with `RUST_BACKTRACE=1 ./rct` to get detailed backtraces on errors (may require explicitly using std::backtrace::Backtrace in your code).

## Project Overview

1.
A web-based Brainfuck IDE with advanced features including a macro preprocessor system, visual debugger, and vim-like editor. 
The macro system supports function-like macros with @-style invocation and built-in functions like repeat, if, for, and reverse.
2.
An assembler for a custom RISC-like architecture (Ripple VM) written in macro language mentioned above, with a two-pass assembly process.
3.
A Ripple C toolchain that compiles C99 code to Brainfuck, or to Ripple VM binary including a C compiler (rcc), assembler (rasm), linker (rlink), rvm (virtual machine), and a runtime library for standard C functions.

## Ripple Assembler

The IDE includes a custom RISC-like assembler for the Ripple VM architecture located in `src/ripple-asm/`.

### Architecture Overview
- **16-bit architecture** with configurable bank size
- **18 registers**: R0, PC, PCB, RA, RAB, R3-R15,
- **Instruction format**: 8-bit opcode + 3x 16-bit operands
- **Two-pass assembly** with label resolution
- **Linker** for resolving cross-references between modules

- **Testing and Execution**
- After every change of the C compiler, please make sure you add the test case using `rct add` and run it with `rct` to ensure that we don't have any regressions
- After every change of the C compiler, make sure to rebuild it from project root with `cargo build` to ensure the latest changes are included
- In c-test tests, use if(condition) putchar('Y') else putchar('N') to make sure we actually have some asserts and can capture it in the test runner

## Developing C Compiler

When working on the C compiler, follow these steps:
1. Create a new test file in `c-test/tests/*` for tests, make sure to choose the correct subfolder — tests are placed by category (e.g. tests/core/typedef). Use `rct list -c` to see available categories.
2. Add the test to the suite using `./rct add tests/test_name.c "expected output"`. To specify newline, use "\\n"
3. Implement compiler logic
4. Run the test suite with `./rct test_name` to verify changes.
5. Ensure all tests pass.
6. If tests fail, debug using the generated files in `c-test/build/`.

## Directory Structure

```
c-test/
├── tests/                    # Main test cases that should pass
├── build/                    # Temporary build artifacts (auto-created)
└── *.meta.json              # Test metadata files (created by rct add)

rcc-test/
├── src/
│   ├── main.rs              # Main entry point for rct
│   ├── cli.rs               # CLI argument parsing
│   └── ...                  # Test runner implementation
└── Cargo.toml               # Rust dependencies
```

## Running Tests

```bash
# Run all tests
rct

# Run specific tests (without path or .c extension)
rct test_example test_example2

# Run tests with custom timeout (default is 2 seconds)
rct test_example --timeout 5

```

## Adding a New Test

### 1. Create a Test File

Create a `.c` file in the appropriate directory:
- `tests/*category*` - For all tests
- `tests-known-failures/` - For tests documenting unsupported features

### 2. Write Test Code

Tests should output predictable text that can be verified. Use `putchar()` for single char output, puts for strings.

```c
// tests/test_example.c
#include <stdio.h>

int main() {
    puts("Hello, World!");  // Expected output: "Hello, World!\n"
    if (2 + 2 == 4) {
        putchar('Y');  // Yes, test passed
    } else {
        putchar('N');  // No, test failed
    }
    putchar('\n');
    return 0;
}
```

### 3. Add Test to Test Suite

```bash
# Add a regular test with expected output
rct add tests/test_new.c "Hello\\n"

# Add a test with description
rct add tests/test_new.c "Hello\\n" -d "Test greeting output"
```

The `rct add` command creates a `.meta.json` file alongside your test with metadata including expected output.

### 4. Run Your Test

```bash
# Run specific test
rct test_new

# Run all tests
rct

# Run tests with a specific category
rct -c core/typedef

```

## Test Guidelines

1. **Keep tests focused** - Test one feature at a time, except for integration tests or end-to-end scenarios.
2. **Use assertions** - Output 'Y' for pass, 'N' for fail conditions
3. **Include newlines** - End output with `\n` for clean formatting

Rct preserves in `build/`:
- `.ir` - Intermediate representation files
- `.asm` - Generated assembly
- `.pobj` - Assembled object files
- `.bin` - Final binary output (RVM backend)

You can then manually inspect or run individual compilation steps.

### Additional Test Management Commands

```bash
# List all tests
rct list

# List all categories
rct list -c

# Rename a test
rct rename old_name new_name
```



IMPORTANT: Use `rcc compile file.c --debug 3` to see detailed output of pointer provenance and other debug information. It is VERY helpful.
IMPORTANT: Use "log" crate's `trace!` and `debug!` macros to log detailed information during compilation. This will help you understand how the compiler processes your code.

