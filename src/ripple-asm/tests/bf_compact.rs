//! Compare direct BF export with macro expansion using an independent literal
//! 16-bit interpreter. No native Ripple or optimized BF engine is involved.
use bf_macro_expander::{MacroExpander, MacroExpanderOptions};
use ripple_asm::{
    AssemblerOptions, Instruction, LinkedProgram, Linker, MacroFormatter, Opcode, RippleAssembler,
};
use std::io::{self, Write};

const ORIGIN: usize = 8;
const LANES: usize = 8;

#[derive(Clone, Copy)]
enum Op {
    Add(u16),
    Move(isize),
    Open(usize),
    Close(usize),
    Clear,
    Hook(u8),
    Output,
}

fn parse(code: &str) -> Vec<Op> {
    let bytes: Vec<_> = code
        .bytes()
        .filter(|byte| b"+-<>[],.$?!".contains(byte))
        .collect();
    let (mut ops, mut stack, mut at) = (Vec::new(), Vec::new(), 0);
    while at < bytes.len() {
        if bytes[at..].starts_with(b"[-]") {
            ops.push(Op::Clear);
            at += 3;
            continue;
        }
        let byte = bytes[at];
        match byte {
            b'+' | b'-' => {
                let mut delta = 0u16;
                while at < bytes.len() && b"+-".contains(&bytes[at]) {
                    delta = delta.wrapping_add(if bytes[at] == b'+' { 1 } else { u16::MAX });
                    at += 1;
                }
                ops.push(Op::Add(delta));
                continue;
            }
            b'>' | b'<' => {
                let mut count = 0;
                // Canceling opposite moves could conceal an out-of-bounds visit.
                while bytes.get(at) == Some(&byte) {
                    count += 1;
                    at += 1;
                }
                ops.push(Op::Move(if byte == b'>' { count } else { -count }));
                continue;
            }
            b'[' => {
                stack.push(ops.len());
                ops.push(Op::Open(0));
            }
            b']' => {
                let start = stack.pop().expect("balanced closing bracket");
                ops[start] = Op::Open(ops.len());
                ops.push(Op::Close(start));
            }
            b'.' => ops.push(Op::Output),
            b',' => panic!("host exports must use input hooks"),
            _ => ops.push(Op::Hook(byte)),
        }
        at += 1;
    }
    assert!(stack.is_empty(), "balanced opening brackets");
    ops
}

fn legacy(image: &LinkedProgram, banks: u16) -> String {
    let macros = MacroFormatter::new()
        .format_linked_program_host(image, banks)
        .unwrap();
    let expansion = MacroExpander::new().expand(
        &macros,
        MacroExpanderOptions {
            strip_comments: true,
            ..Default::default()
        },
    );
    assert!(expansion.errors.is_empty(), "{:?}", expansion.errors);
    expansion.expanded
}

fn compact(image: &LinkedProgram, banks: u16) -> String {
    let mut bytes = Vec::new();
    MacroFormatter::new()
        .write_linked_program_host_bf(image, banks, &mut bytes)
        .unwrap();
    String::from_utf8(bytes).unwrap()
}

fn split_image(source: &str) -> (Vec<&str>, &str) {
    let (header, code) = source.split_once("END_BFHOST").unwrap();
    assert!(
        !header.bytes().any(|byte| b"+-<>[],.$?!".contains(&byte)),
        "metadata must be inert in ordinary BF interpreters"
    );
    let words: Vec<_> = header.split_whitespace().collect();
    assert_eq!(&words[..2], &["BFHOST", "1"]);
    (words, code)
}

#[derive(Debug, PartialEq, Eq)]
struct State {
    tape: Vec<u16>,
    pointer: usize,
    hooks: Vec<(u8, usize, u16)>,
    output: Vec<u8>,
}

fn run(source: &str, stop_at_first_instruction: bool) -> State {
    run_with_ram(source, stop_at_first_instruction, &[])
}

fn run_with_ram(
    source: &str,
    stop_at_first_instruction: bool,
    initial_ram: &[(usize, u16)],
) -> State {
    let (header, code) = split_image(source);
    assert_eq!(header[header.len() - 2], "TAPE_CELLS");
    let cells = header.last().unwrap().parse().unwrap();
    let mut state = State {
        tape: vec![0; cells],
        pointer: 0,
        hooks: Vec::new(),
        output: Vec::new(),
    };
    for &(address, value) in initial_ram {
        state.tape[ORIGIN + address * LANES + 4] = value;
    }
    let ops = parse(code);
    let (mut pc, mut fuel) = (0, 100_000_000u64);
    while pc < ops.len() {
        assert!(fuel != 0, "literal BF operation budget exhausted at {pc}");
        fuel -= 1;
        match ops[pc] {
            Op::Add(delta) => {
                state.tape[state.pointer] = state.tape[state.pointer].wrapping_add(delta);
            }
            Op::Move(delta) => {
                state.pointer = state
                    .pointer
                    .checked_add_signed(delta)
                    .expect("strict lower tape bound");
                assert!(state.pointer < cells, "strict upper tape bound");
            }
            Op::Clear => state.tape[state.pointer] = 0,
            Op::Open(end) if state.tape[state.pointer] == 0 => pc = end,
            Op::Close(start) if state.tape[state.pointer] != 0 => pc = start,
            Op::Hook(marker) => {
                state
                    .hooks
                    .push((marker, state.pointer, state.tape[state.pointer]));
                if marker == b'$' && stop_at_first_instruction {
                    return state;
                }
                if marker == b'?' && state.pointer == ORIGIN + LANES * 31 + 4 {
                    // A deterministic device reply tests resuming after a hook.
                    state.tape[state.pointer] = 65;
                }
                if marker == b'!' && state.pointer == ORIGIN + 4 {
                    state.output.push(state.tape[state.pointer] as u8);
                }
            }
            Op::Output => panic!("host export must not duplicate TTY output"),
            _ => {}
        }
        pc += 1;
    }
    assert!(!stop_at_first_instruction, "missing first instruction hook");
    assert_eq!(state.tape[ORIGIN], 0, "CPU must halt");
    assert!(state.tape[..ORIGIN].iter().all(|&cell| cell == 0));
    state
}

fn image(bank_size: u16, entry: usize, instructions: Vec<Instruction>) -> LinkedProgram {
    LinkedProgram {
        instructions,
        data: vec![],
        bf_host_gp_data_only: true,
        labels: Default::default(),
        data_labels: Default::default(),
        entry_point: (entry * 4) as u32,
        bank_size,
        relocs: vec![],
    }
}

fn assemble(source: &str) -> LinkedProgram {
    let options = AssemblerOptions {
        bank_size: 37,
        max_immediate: u32::from(u16::MAX),
        ..Default::default()
    };
    let object = RippleAssembler::new(options).assemble(source).unwrap();
    Linker::new(37).link(vec![object]).unwrap()
}

#[test]
fn compact_initialization_matches_full_tape_at_zero_nonzero_and_later_bank_entry() {
    let edges = [0, 1, 127, 128, 255, 256, 257, 32767, 32768, 65534, 65535];
    let mut code: Vec<_> = (0..48)
        .map(|i| {
            Instruction::new(
                Opcode::Li,
                (i % 32) as u16,
                edges[i % edges.len()],
                edges[edges.len() - 1 - i % edges.len()],
            )
        })
        .collect();
    // The direct exporter must retain the macro CPU's opcode encoding too.
    for opcode in [
        Opcode::Halt,
        Opcode::Nop,
        Opcode::Add,
        Opcode::Sub,
        Opcode::And,
        Opcode::Or,
        Opcode::Xor,
        Opcode::Sll,
        Opcode::Srl,
        Opcode::Slt,
        Opcode::Sltu,
        Opcode::Addi,
        Opcode::Andi,
        Opcode::Ori,
        Opcode::Xori,
        Opcode::Li,
        Opcode::Slli,
        Opcode::Srli,
        Opcode::Load,
        Opcode::Store,
        Opcode::Jal,
        Opcode::Jalr,
        Opcode::Beq,
        Opcode::Bne,
        Opcode::Blt,
        Opcode::Bge,
        Opcode::Brk,
        Opcode::Mul,
        Opcode::Div,
        Opcode::Mod,
        Opcode::Muli,
        Opcode::Divi,
        Opcode::Modi,
    ] {
        code.push(Instruction::new(opcode, 7, 7, 3));
    }
    for (entry, banks) in [(0, 1), (1, 4), (40, 16)] {
        let image = image(37, entry, code.clone());
        let old = legacy(&image, banks);
        let new = compact(&image, banks);
        assert_eq!(split_image(&old).0, split_image(&new).0);
        let expected = run(&old, true);
        let actual = run(&new, true);
        assert_eq!(actual, expected, "entry={entry}, banks={banks}");
        assert_eq!(actual.pointer, ORIGIN + LANES * (40 + 4 * entry));
        assert!(actual.tape[..ORIGIN].iter().all(|&cell| cell == 0));
        // Initializer scratch must not leak into CPU execution or RAM.
        for word in actual.tape[ORIGIN..].chunks_exact(LANES) {
            assert_eq!(word[4], 0, "RAM starts clear");
            assert_eq!((word[5], word[6]), (0, 0), "scratch starts clear");
        }
        // The host populates MMIO before BF initialization. Neither export may
        // erase those values while preparing code and scratch in adjacent lanes.
        let initial_ram = [
            (0, 0x565),
            (31, u16::MAX),
            (usize::from(banks) * 37 - 1, 32768),
        ];
        let expected = run_with_ram(&old, true, &initial_ram);
        let actual = run_with_ram(&new, true, &initial_ram);
        assert_eq!(actual, expected, "prepopulated RAM at entry={entry}");
        for (address, value) in initial_ram {
            assert_eq!(actual.tape[ORIGIN + address * LANES + 4], value);
        }
    }
}

#[test]
fn compact_execution_matches_all_hooks_output_and_final_tape() {
    let fixtures = [
        "start:\nLI T0, 31\nLOAD A0, R0, T0\nSTORE A0, R0, R0\nBRK\nHALT\n"
            .to_string(),
        "HALT\nstart:\nLI T0, 7\nLI T1, 2\nLI A0, 66\nSTORE A0, T1, T0\nLOAD A1, T1, T0\nSTORE A1, R0, R0\nHALT\n".to_string(),
        format!(
            "start:\nCALL far\nSTORE A0, R0, R0\nHALT\n{}far:\nLI A0, 67\nRET\n",
            "NOP\n".repeat(34)
        ),
    ];
    for (index, source) in fixtures.iter().enumerate() {
        let image = assemble(source);
        let expected = run(&legacy(&image, 4), false);
        let actual = run(&compact(&image, 4), false);
        assert_eq!(actual, expected, "fixture={index}");
        assert_eq!(actual.output, [b'A' + index as u8]);
    }
}

#[test]
fn large_constant_image_is_compact_without_changing_initialized_values() {
    let high = image(
        37,
        0,
        (0..64)
            .map(|i| Instruction::new(Opcode::Li, 7, 32767 + i, 0))
            .collect(),
    );
    let zero = image(37, 0, vec![Instruction::new(Opcode::Li, 7, 0, 0); 64]);
    let old = legacy(&high, 1);
    let new = compact(&high, 1);
    // The unchanged CPU body has a significant fixed size. Compare the extra
    // cost of encoding constants with the same image containing zero values.
    let old_constants = old.len() - legacy(&zero, 1).len();
    let new_constants = new.len() - compact(&zero, 1).len();
    assert!(
        new_constants < old_constants / 16,
        "compact constants={new_constants} bytes, macro constants={old_constants} bytes"
    );
    assert!(new_constants < 64 * 512);
    assert!(new.len() < old.len());
    assert_eq!(run(&new, true), run(&old, true));
}

#[test]
fn initialization_cleanup_crosses_the_16_bit_word_boundary() {
    let mut code = vec![Instruction::new(Opcode::Nop, 0, 0, 0); 17_000];
    code[0] = Instruction::new(Opcode::Halt, 0, 0, 0);
    let image = image(37, 0, code);
    // This direct-export-only check stays small while placing cleanup and
    // adjacent host RAM values on both sides of a 16-bit word counter boundary.
    let source = compact(&image, 2048);
    let initial_ram = [
        (65534, 0x1111),
        (65535, u16::MAX),
        (65536, 32768),
        (65537, 1),
    ];
    let state = run_with_ram(&source, true, &initial_ram);
    assert_eq!(state.pointer, ORIGIN + LANES * 40);
    assert_eq!(state.hooks.len(), 1);
    assert!(state.tape[..ORIGIN].iter().all(|&cell| cell == 0));
    for (index, instruction) in image.instructions.iter().enumerate() {
        let opcode_cell = ORIGIN + LANES * (40 + 4 * index) + 3;
        assert_eq!(state.tape[opcode_cell], u16::from(instruction.opcode));
        for operand in 1..=3 {
            assert_eq!(state.tape[opcode_cell - operand * LANES], 0);
        }
    }
    for (word_index, word) in state.tape[ORIGIN..].chunks_exact(LANES).enumerate() {
        if word_index != 0 {
            assert_eq!(word[0], 0, "initialization marker at word {word_index}");
        }
        assert_eq!(
            (word[2], word[5], word[6]),
            (0, 0, 0),
            "scratch at word {word_index}"
        );
    }
    for (address, value) in initial_ram {
        assert_eq!(state.tape[ORIGIN + address * LANES + 4], value);
    }
}

#[test]
fn compact_validation_matches_macro_export_and_writes_nothing_on_invalid_input() {
    let valid = || image(37, 0, vec![Instruction::new(Opcode::Halt, 0, 0, 0)]);
    let mut invalid = Vec::new();
    invalid.push((valid(), 0));
    let mut case = valid();
    case.bank_size = 31;
    invalid.push((case, 1));
    let mut case = valid();
    case.bf_host_gp_data_only = false;
    case.data.push(42);
    invalid.push((case, 1));
    for entry in [1, 4, u32::MAX] {
        let mut case = valid();
        case.entry_point = entry;
        invalid.push((case, 1));
    }
    let mut case = valid();
    case.instructions.clear();
    invalid.push((case, 1));
    for opcode in [Opcode::Loadc, Opcode::Storc] {
        let mut case = valid();
        case.instructions[0] = Instruction::new(opcode, 0, 0, 0);
        invalid.push((case, 1));
    }
    for instruction in [
        Instruction::new(Opcode::Li, 32, 0, 0),
        Instruction::new(Opcode::Add, 7, 7, 32),
    ] {
        let mut case = valid();
        case.instructions[0] = instruction;
        invalid.push((case, 1));
    }
    let formatter = MacroFormatter::new();
    for (image, banks) in invalid {
        let expected = formatter
            .format_linked_program_host(&image, banks)
            .unwrap_err();
        let mut output = Vec::new();
        let actual = formatter
            .write_linked_program_host_bf(&image, banks, &mut output)
            .unwrap_err();
        assert_eq!(actual, expected);
        assert!(output.is_empty(), "validation must precede writing");
    }
}

#[test]
fn compact_export_propagates_writer_failures() {
    struct FailingWriter;
    impl Write for FailingWriter {
        fn write(&mut self, _: &[u8]) -> io::Result<usize> {
            Err(io::Error::other("intentional output failure"))
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }
    let image = image(37, 0, vec![Instruction::new(Opcode::Halt, 0, 0, 0)]);
    let error = MacroFormatter::new()
        .write_linked_program_host_bf(&image, 1, &mut FailingWriter)
        .unwrap_err();
    assert!(error.contains("intentional output failure"), "{error}");
}
