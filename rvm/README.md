# RVM - Ripple Virtual Machine

A reference implementation of the Ripple VM architecture that executes programs produced by the ripple-asm assembler and linker.

## Features

- Complete implementation of all Ripple VM instructions
- Support for 18 registers (R0-R15, PC, PCB, RA, RAB)
- 16-bit cell width with wrap-around arithmetic  
- Configurable bank size (default: 4096)
- Memory-mapped I/O for output
- Debug mode for step-by-step execution
- Binary program loader (RLINK format)

## Building

```bash
cargo build --release
```

## Usage

```bash
# Run a binary program
rvm program.bin

# Run with verbose output
rvm -v program.bin

# Run in debug mode (step-by-step)
rvm -d program.bin

# Specify custom bank size
rvm -b 4096 program.bin

# Specify custom memory size (in words)
rvm -m 32768 program.bin

# Or use cargo run
cargo run --release -- program.bin
```

### Debug Mode Commands

When running with `-d` (simple debugger), the following commands are available:
- **Enter** - Step one instruction
- **r** - Run to completion
- **c** - Continue from breakpoint (after BRK)
- **q** - Quit debugger

## TUI Debugger Mode (-t)

The TUI (Terminal User Interface) debugger provides a professional debugging environment with multiple synchronized panes and advanced features.

### Launch
```bash
rvm -t program.bin
```

### Interface Layout
The TUI debugger displays 6 panes simultaneously:
1. **Disassembly** (F1) - Shows instructions with addresses, breakpoints, and current PC
2. **Registers** (F2) - All 18 registers with change highlighting
3. **Memory** (F3) - Hex and ASCII view with editing capabilities
4. **Stack** (F4) - Return address tracking
5. **Watches** (F5) - Named memory locations for monitoring
6. **Output** (F6) - Program output buffer

### Navigation
- **F1-F6** - Switch directly to specific pane
- **Tab** - Cycle through panes forward
- **Shift+Tab** - Cycle through panes backward
- **h/j/k/l** - Vim-style navigation within panes
- **↑/↓/←/→** - Arrow key navigation
- **PageUp/PageDown** - Scroll by page in current pane

### Execution Control
- **Space/s** - Step single instruction
- **r** - Run until breakpoint or halt
- **c** - Continue from current breakpoint
- **R** - Restart execution from beginning (preserves program)

### Breakpoints
- **b** - Toggle breakpoint at cursor position
- **Shift+B** - Set/toggle breakpoint by instruction number
  - Enter instruction number (e.g., "5" for instruction #5)
  - Or hex address (e.g., "0x100")
- **B** - Clear all breakpoints

### Memory Operations
- **g** - Go to memory address (enter hex address)
- **a** - Toggle ASCII display in memory pane
- **e** - Edit memory with multiple formats:
  - `addr:0xFF` - Hexadecimal value
  - `addr:255` - Decimal value
  - `addr:'A'` - Single character
  - `addr:"Hello"` - String (writes multiple bytes)
  - `addr:0b1010` - Binary value
- **0-9,a-f** - Quick hex edit at cursor (Memory pane only)
- **w** - Add memory watch (format: `name:address[:format]`)
- **W** - Remove selected watch

### Command Mode (:)
Press `:` to enter command mode for advanced operations:
- `:break <addr>` - Set breakpoint at address
- `:delete <addr>` - Remove breakpoint at address
- `:watch <name> <addr>` - Add named memory watch
- `:mem <addr> <value>` - Write value to memory
- `:reg <#> <value>` - Set register value
- `:help` - Show help
- `:quit` or `:q` - Exit debugger

### Other Keys
- **?** - Toggle help overlay
- **q** - Quit debugger (also available in command mode)
- **ESC** - Cancel current input/mode

### Visual Indicators
- **Breakpoints** - Red `●` markers in disassembly
- **Current PC** - Yellow highlighted line with `►` marker
- **Register changes** - Yellow highlighting for recently modified registers
- **Non-zero values** - White text for non-zero memory/registers
- **I/O registers** - Magenta highlighting for addresses 0x0000-0x0001
- **Active pane** - Cyan border and "ACTIVE" label

### Memory Display Features
- 16 bytes per row in hexadecimal
- Optional ASCII representation (toggle with 'a')
- Color coding for special addresses and non-zero values
- Continuous scrolling through entire memory space

### Tips
- Use Shift+B to quickly set breakpoints at specific instructions
- Memory edits support various formats for convenience
- Command history available with ↑/↓ in command mode
- All numeric inputs accept hex (0x prefix) or decimal

## Testing

Run the test suite:

```bash
./run_tests.sh
```

The test suite includes:
- `test_hello` - Basic I/O and HALT
- `test_arithmetic` - ADD, MUL, and loops
- `test_bitwise` - AND, OR, XOR, shifts
- `test_comparison` - SLT, SLTU, BLT, BGE
- `test_division` - DIV, MOD and immediate versions
- `test_jumps` - CALL/RET (JAL/JALR)
- `test_memory` - LOAD/STORE operations
- `test_loadc_storc` - LOADC/STORC instruction-memory copies

## Architecture

The VM implements the Ripple architecture as specified in `docs/ASSEMBLY_FORMAT.md`:

### Registers
- R0: Always reads as 0
- PC: Program counter (offset within bank)
- PCB: Program counter bank
- RA: Return address (low)
- RAB: Return address bank (high)
- R3-R15: General purpose

### Memory
- 64KB address space (65536 16-bit cells)
- Memory-mapped I/O:
  - 0x0000: Output register (write byte to stdout)
  - 0x0001: Output ready flag

### Instruction Set
All opcodes from 0x00 to 0x21 are implemented, including:
- ALU operations (ADD, SUB, AND, OR, XOR, SLL, SRL, SLT, SLTU)
- Immediate operations (ADDI, ANDI, ORI, XORI, LI, SLLI, SRLI)
- Data memory operations (LOAD, STORE)
- Instruction memory copies (LOADC, STORC) — X0..X3 ↔ 4 cells at IMEM[bank][addr]; bank and addr are registers
- Control flow (JAL, JALR, BEQ, BNE, BLT, BGE)
- Extended arithmetic (MUL, DIV, MOD and immediate versions)
- Special (NOP, HALT, BRK)

### BRK Instruction Behavior

The BRK (breakpoint) instruction behaves differently depending on the execution mode:

#### Normal Mode (without -d flag)
- Dumps complete VM state to stderr including:
  - All register values (hex and decimal)
  - First 32 words of memory
  - Current instruction details
- **Halts execution immediately**
- Useful for debugging crashes and inspecting state

#### Debug Mode (with -d flag)
- Prints breakpoint notification
- **Pauses execution** without halting
- Allows continuing with 'c' command or single-stepping
- Acts as a traditional debugger breakpoint

## Program Format

The VM loads binary programs in RLINK format:
- Magic: "RLINK" (5 bytes)
- Entry point (4 bytes, little-endian)
- Instruction count (4 bytes, little-endian)
- Instructions (8 bytes each)
- Data size (4 bytes, little-endian)
- Data section (variable length)