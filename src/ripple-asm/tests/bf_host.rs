//! Bounded execution of the generated BF CPU, independent of the native host.
use bf_macro_expander::{MacroExpander, MacroExpanderOptions};
use ripple_asm::{AssemblerOptions, Linker, MacroFormatter, RippleAssembler};

#[derive(Debug)]
enum Op {
    Add(i32),
    Move(i32),
    Open(usize),
    Close(usize),
    Clear,
    Hook(u8),
    Output,
}

fn expand(source: &str, bank_size: u16, banks: u16) -> Result<String, String> {
    let object = RippleAssembler::new(AssemblerOptions::default())
        .assemble(source)
        .unwrap();
    let program = Linker::new(bank_size).link(vec![object]).unwrap();
    let bfm = MacroFormatter::new().format_linked_program_host(&program, banks)?;
    let result = MacroExpander::new().expand(
        &bfm,
        MacroExpanderOptions {
            strip_comments: true,
            ..Default::default()
        },
    );
    assert!(result.errors.is_empty(), "{:?}", result.errors);
    Ok(result.expanded)
}

fn parse(code: &str) -> Vec<Op> {
    let bytes: Vec<_> = code.bytes().filter(|c| b"+-<>[].$?!".contains(c)).collect();
    let mut ops = Vec::new();
    let mut stack = Vec::new();
    let mut i = 0;
    while i < bytes.len() {
        let c = bytes[i];
        if c == b'[' && bytes.get(i + 1) == Some(&b'-') && bytes.get(i + 2) == Some(&b']') {
            ops.push(Op::Clear);
            i += 3;
            continue;
        }
        if b"+-<>".contains(&c) {
            let movement = c == b'<' || c == b'>';
            let mut delta = 0;
            while i < bytes.len()
                && (if movement {
                    b"<>".contains(&bytes[i])
                } else {
                    b"+-".contains(&bytes[i])
                })
            {
                delta += if bytes[i] == b'>' || bytes[i] == b'+' {
                    1
                } else {
                    -1
                };
                i += 1;
            }
            ops.push(if movement {
                Op::Move(delta)
            } else {
                Op::Add(delta)
            });
            continue;
        }
        match c {
            b'[' => {
                stack.push(ops.len());
                ops.push(Op::Open(0));
            }
            b']' => {
                let start = stack.pop().unwrap();
                let end = ops.len();
                ops[start] = Op::Open(end);
                ops.push(Op::Close(start));
            }
            b'.' => ops.push(Op::Output),
            _ => ops.push(Op::Hook(c)),
        }
        i += 1;
    }
    assert!(stack.is_empty());
    ops
}

struct ResultState {
    tape: Vec<u16>,
    reads: Vec<usize>,
    writes: Vec<(usize, u16)>,
    boundaries: usize,
}

fn run(source: &str) -> ResultState {
    let expanded = expand(source, 64, 4).unwrap();
    let (header, code) = expanded.split_once("END_BFHOST").unwrap();
    let words: Vec<_> = header.split_whitespace().collect();
    assert_eq!(
        &words[..6],
        &["BFHOST", "1", "BANK_SIZE", "64", "MEMORY_BANKS", "4"]
    );
    assert!(!header.bytes().any(|c| b"+-<>[],.$?!".contains(&c)));
    let cells: usize = words.last().unwrap().parse().unwrap();
    let ops = parse(code);
    let mut result = ResultState {
        tape: vec![0; cells],
        reads: vec![],
        writes: vec![],
        boundaries: 0,
    };
    let (mut ptr, mut pc, mut fuel) = (0usize, 0usize, 100_000_000u64);
    while pc < ops.len() {
        assert!(
            fuel > 0,
            "BF test exhausted its operation budget at op {pc}, ptr {ptr}"
        );
        fuel -= 1;
        match ops[pc] {
            Op::Add(n) => result.tape[ptr] = result.tape[ptr].wrapping_add(n as u16),
            Op::Move(n) => {
                ptr = usize::try_from(ptr as i64 + i64::from(n)).unwrap_or_else(|_| {
                    panic!(
                        "negative tape pointer: ptr={ptr}, delta={n}, pc={pc}, boundaries={}",
                        result.boundaries
                    )
                });
                assert!(ptr < cells, "tape overflow {ptr} >= {cells}");
            }
            Op::Open(end) if result.tape[ptr] == 0 => pc = end,
            Op::Close(start) if result.tape[ptr] != 0 => pc = start,
            Op::Clear => result.tape[ptr] = 0,
            Op::Hook(b'$') => {
                let instruction =
                    usize::from(result.tape[8 + 25]) * 64 + usize::from(result.tape[8 + 17]);
                assert_eq!(ptr, 8 + 8 * (40 + 4 * instruction));
                result.boundaries += 1;
            }
            Op::Hook(marker) => {
                assert_eq!(ptr % 8, 4, "hook must point at a RAM cell");
                let address = (ptr - 8) / 8;
                assert!(address < 256);
                if marker == b'?' {
                    result.reads.push(address);
                    if address == 31 {
                        result.tape[ptr] = 1;
                    }
                } else {
                    result.writes.push((address, result.tape[ptr]));
                }
            }
            Op::Output => panic!("host export must not duplicate TTY output"),
            _ => {}
        }
        pc += 1;
    }
    assert_eq!(
        result.tape[8], 0,
        "CPU should halt, ptr={ptr}, boundaries={}, writes={:?}, PC={}, RA={}",
        result.boundaries, result.writes, result.tape[25], result.tape[41]
    );
    assert!(
        result.tape[..8].iter().all(|&cell| cell == 0),
        "leading guard must remain clear"
    );
    result
}

#[test]
fn memory_banks_and_atomic_host_hooks() {
    let state = run("start:\nLI T0, 7\nLI T1, 1\nLI A0, 11\nSTORE A0, T1, T0\nLI T1, 2\nLI A0, 22\nSTORE A0, T1, T0\nLI T1, 3\nLI A0, 33\nSTORE A0, T1, T0\nLI T1, 1\nLOAD S0, T1, T0\nLI T1, 2\nLOAD S1, T1, T0\nLI T1, 3\nLOAD S2, T1, T0\nLI T0, 31\nLOAD S3, R0, T0\nLI A0, 65\nSTORE A0, R0, R0\nSTORE A0, R0, R0\nHALT\n");
    assert_eq!(
        state.writes,
        [(71, 11), (135, 22), (199, 33), (0, 65), (0, 65)]
    );
    assert_eq!(state.reads, [71, 135, 199, 31]);
    for (register, value) in [(23, 11), (24, 22), (25, 33), (26, 1)] {
        assert_eq!(state.tape[8 + 8 * (register + 1) + 1], value);
    }
    for address in [70, 72, 134, 136, 198, 200] {
        assert_eq!(state.tape[8 + address * 8 + 4], 0);
    }
    assert_eq!(state.boundaries, 22);
}

#[test]
fn nonzero_entry_and_branches_execute_in_bf() {
    let state = run("HALT\nstart:\nLI T0, 3\nLI A0, 0\nloop:\nADDI A0, A0, 1\nADDI T0, T0, -1\nBNE T0, R0, loop\nSTORE A0, R0, R0\nBRK\nHALT\n");
    assert_eq!(state.writes, [(0, 3)]);
    assert_eq!(state.boundaries, 14); // BRK is a no-op, not an extra host boundary.
}

#[test]
fn arithmetic_and_logic_smoke() {
    let cases = [
        ("ADD A0, T0, T1", 15),
        ("SUB A0, T0, T1", 9),
        ("AND A0, T0, T1", 0),
        ("OR A0, T0, T1", 15),
        ("XOR A0, T0, T1", 15),
        ("SLL A0, T0, T1", 96),
        ("SRL A0, T0, T1", 1),
        ("SLT A0, T0, T1", 0),
        ("SLTU A0, T0, T1", 0),
        ("MUL A0, T0, T1", 36),
        ("DIV A0, T0, T1", 4),
        ("MOD A0, T0, T1", 0),
        ("ADDI A0, T0, -1", 11),
        ("ANDI A0, T0, 3", 0),
        ("ORI A0, T0, 3", 15),
        ("XORI A0, T0, 3", 15),
        ("SLLI A0, T0, 2", 48),
        ("SRLI A0, T0, 2", 3),
        ("MULI A0, T0, 3", 36),
        ("DIVI A0, T0, 3", 4),
        ("MODI A0, T0, 5", 2),
    ];
    let mut source = "start:\nLI T0, 12\nLI T1, 3\n".to_string();
    for (instruction, _) in cases {
        source.push_str(&format!("{instruction}\nSTORE A0, R0, R0\n"));
    }
    source.push_str("NOP\nHALT\n");
    let state = run(&source);
    assert_eq!(state.writes, cases.map(|(_, value)| (0, value)));
}

#[test]
fn signed_branch_edges() {
    let state = run("start:\nLI T0, -32768\nLI T1, 1\nLI A0, 0\nBLT T0, T1, less\nLI A0, 99\nless:\nSTORE A0, R0, R0\nBGE T1, T0, greater\nLI A0, 98\ngreater:\nSTORE A0, R0, R0\nHALT\n");
    assert_eq!(state.writes, [(0, 0), (0, 0)]);
}

#[test]
fn jump_link_destination_and_nonlinking_return() {
    let state = run("start:\nLI RA, 7\nLI RAB, 9\nJAL R0, R0, no_link\nHALT\nno_link:\nSTORE RA, R0, R0\nSTORE RAB, R0, R0\nJAL A3, R0, linked\nHALT\nlinked:\nSTORE A3, R0, R0\nSTORE RA, R0, R0\nSTORE RAB, R0, R0\nHALT\n");
    assert_eq!(state.writes, [(0, 7), (0, 9), (0, 7), (0, 7), (0, 0)]);
    // JALR snapshots its target before writing an aliased link destination.
    let state = run("start:\nLI RA, 7\nLI RAB, 9\nLI T0, 5\nJALR T0, R0, T0\nHALT\nSTORE T0, R0, R0\nSTORE RA, R0, R0\nSTORE RAB, R0, R0\nNOP\nLI T0, 13\nLI RAB, 9\nJALR R0, R0, T0\nHALT\nSTORE RA, R0, R0\nSTORE RAB, R0, R0\nHALT\n");
    assert_eq!(state.writes, [(0, 4), (0, 7), (0, 0), (0, 7), (0, 9)]);
}

#[test]
fn zero_register_ignores_writes() {
    let state = run("start:\nLI R0, 5\nLI A0, 7\nADD A1, R0, A0\nLI T0, 0\nSTORE A1, T0, T0\nADDI R0, A0, 9\nADD A1, R0, A0\nSTORE A1, T0, T0\nHALT\n");
    assert_eq!(state.writes, [(0, 7), (0, 7)]);
}

#[test]
fn unsupported_images_fail_before_expansion() {
    for opcode in ["LOADC", "STORC"] {
        assert!(
            expand(&format!("start:\n{opcode} R0, T0, T1\nHALT\n"), 64, 4)
                .unwrap_err()
                .contains("LOADC/STORC")
        );
    }
    assert!(expand("HALT", 16, 4).unwrap_err().contains("at least 32"));
    assert!(expand("HALT", 64, 0).unwrap_err().contains("at least one"));
    assert!(expand("HALT\n.data\nvalue: .byte 1", 64, 4)
        .unwrap_err()
        .contains("legacy .data"));
}

#[test]
fn multiple_code_banks_execute_calls_in_brainfuck() {
    let source = format!(
        "start:\nCALL far\nSTORE A0, R0, R0\nHALT\n{}far:\nLI A0, 65\nRET\n",
        "NOP\n".repeat(61)
    );
    assert_eq!(run(&source).writes, [(0, 65)]);
    assert_eq!(run_plain(&source).0, b"A");
}

#[test]
fn maximum_raw_pc_rolls_into_the_next_code_bank() {
    // RVM advances in wider arithmetic: 65535+1 must roll PCB, not merely
    // wrap PC to zero while leaving the bank unchanged.
    let source = format!(
        "start:\nLI PC, -1\nHALT\n{}next_bank:\nLI A0, 65\nSTORE A0, R0, R0\nHALT\n",
        "NOP\n".repeat(62)
    );
    assert_eq!(run(&source).writes, [(0, 65)]);
}

#[test]
fn compiler_gp_reservations_are_not_legacy_mmio_data() {
    let compiler_shape =
        ".data\n__gp_sample:\nvalue:\n.space 1\n.code\n__rcc_init_sample:\nRET\nstart:\nHALT\n";
    assert!(expand(compiler_shape, 64, 4).is_ok());
    // A coincidentally zero legacy data object must not inherit the C object's
    // GP classification when both kinds of objects are linked together.
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    let compiler = assembler.assemble(compiler_shape).unwrap();
    let legacy = assembler.assemble(".data\nlegacy:\n.space 1\n").unwrap();
    let program = Linker::new(64).link(vec![compiler, legacy]).unwrap();
    assert!(MacroFormatter::new()
        .format_linked_program_host(&program, 4)
        .unwrap_err()
        .contains("legacy .data"));
}

// This deliberately understands only standard Brainfuck, with 16-bit cells
// and a circular tape. No host metadata, device hooks, or Ripple decoding are
// involved. Small programs keep the unoptimized scan loops within this budget.
fn run_plain(source: &str) -> (Vec<u8>, Vec<u16>) {
    let object = RippleAssembler::new(AssemblerOptions::default())
        .assemble(source)
        .unwrap();
    let program = Linker::new(64).link(vec![object]).unwrap();
    let bfm = MacroFormatter::new().format_linked_program_standalone(&program, false);
    let expanded = MacroExpander::new().expand(
        &bfm,
        MacroExpanderOptions {
            strip_comments: true,
            ..Default::default()
        },
    );
    assert!(expanded.errors.is_empty(), "{:?}", expanded.errors);
    assert!(!expanded.expanded.contains("BFHOST"));
    assert!(!expanded.expanded.bytes().any(|c| b"?!".contains(&c)));
    // Ordinary interpreters ignore debug '$' annotations, like other comments.
    let code: String = expanded
        .expanded
        .chars()
        .filter(|c| "+-<>[],.".contains(*c))
        .collect();
    assert!(!code.contains(','), "these programs need no external input");
    let ops = parse(&code);
    let mut tape = vec![0u16; 30_000];
    let mut output = Vec::new();
    let (mut ptr, mut pc, mut fuel) = (0usize, 0usize, 100_000_000u64);
    while pc < ops.len() {
        assert!(fuel > 0, "plain BF operation budget exhausted at {pc}");
        fuel -= 1;
        match ops[pc] {
            Op::Add(n) => tape[ptr] = tape[ptr].wrapping_add(n as u16),
            Op::Move(n) => {
                ptr = (ptr as i64 + i64::from(n)).rem_euclid(tape.len() as i64) as usize;
            }
            Op::Open(end) if tape[ptr] == 0 => pc = end,
            Op::Close(start) if tape[ptr] != 0 => pc = start,
            Op::Clear => tape[ptr] = 0,
            Op::Output => output.push(tape[ptr] as u8),
            Op::Hook(_) => panic!("plain interpreter must not see a hook"),
            _ => {}
        }
        pc += 1;
    }
    assert_eq!(tape[0], 0, "plain CPU must halt at the original origin");
    (output, tape)
}

#[test]
fn plain_export_uses_native_tty_and_nonzero_entry() {
    let (output, _) = run_plain(
        "HALT\nstart:\nLI A0, 65\nSTORE A0, R0, R0\nSTORE A0, R0, R0\nBRK\nLI A0, 10\nSTORE A0, R0, R0\nHALT\n",
    );
    assert_eq!(output, b"AA\n");
}

#[test]
fn plain_export_preserves_wrapping_arithmetic_and_zero_register() {
    let (output, _) = run_plain(
        "start:\nLI A0, 32768\nADD A0, A0, A0\nADDI A0, A0, 65\nLI R0, 3\nADD A0, A0, R0\nSTORE A0, R0, R0\nHALT\n",
    );
    assert_eq!(output, b"A");
}

#[test]
fn plain_export_initializes_c_globals_and_uses_separate_stack_bank() {
    // The C compiler emits allocation-only .space plus an initializer routine.
    // Exercise the real linker's synthesized _init_globals and nested returns.
    let (output, tape) = run_plain(
        ".data\n__gp_plain:\nvalue:\n.space 1\n.code\n\
         __rcc_init_plain:\nLI T0, 65\nLI T1, value\nSTORE T0, GP, T1\nRET\n\
         _start:\nLI GP, 1\nLI SB, 2\nLI SP, 1\nCALL _init_globals\nCALL main\nHALT\n\
         main:\nSTORE RA, SB, SP\nADDI SP, SP, 1\nSTORE RAB, SB, SP\nADDI SP, SP, 1\n\
         LI T0, value\nLOAD A0, GP, T0\nSTORE A0, SB, SP\nLI A0, 0\n\
         LOAD A0, SB, SP\nSTORE A0, R0, R0\nADDI SP, SP, -1\nLOAD RAB, SB, SP\n\
         ADDI SP, SP, -1\nLOAD RA, SB, SP\nLI RV0, 0\nRET\n",
    );
    assert_eq!(output, b"A");
    assert_eq!(tape[8 * (64 + 2) + 4], 65, "global bank 1 offset 2");
    assert_eq!(tape[8 * (128 + 3) + 4], 65, "stack bank 2 offset 3");
    assert_eq!(tape[49], 0, "C return value RV0");
}
