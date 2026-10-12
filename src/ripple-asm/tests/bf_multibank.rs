//! Execute the actual expanded CPU with an independent bounded BF interpreter.
//! No native Ripple instruction dispatch or device callbacks are used here.
use bf_macro_expander::{MacroExpander, MacroExpanderOptions};
use ripple_asm::{
    AssemblerOptions, Instruction, LinkedProgram, Linker, MacroFormatter, Opcode, RippleAssembler,
};

const ORIGIN: usize = 8;
const LANES: usize = 8;

#[derive(Clone, Copy)]
enum Op {
    Add(u16),
    Move(isize),
    Open(usize),
    Close(usize),
    Clear,
    Output,
    Hook(u8),
}

fn parse(source: &str) -> Vec<Op> {
    let bytes: Vec<_> = source
        .bytes()
        .filter(|b| b"+-<>[],.?!$".contains(b))
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
                // Do not cancel opposite moves: that could conceal a bounds bug.
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
            b',' => panic!("these fixtures do not request byte input"),
            _ => ops.push(Op::Hook(byte)),
        }
        at += 1;
    }
    assert!(stack.is_empty());
    ops
}

#[derive(Debug)]
struct State {
    tape: Vec<u16>,
    origin: usize,
    trace: Vec<(u16, u16)>, // architectural (PCB, PC), deliberately not normalized
    output: Vec<u8>,
}

impl State {
    fn register(&self, register: usize) -> u16 {
        self.tape[self.origin + LANES * (register + 1) + 1]
    }
}

fn run(image: &LinkedProgram, host: bool) -> State {
    let formatter = MacroFormatter::new();
    let macros = if host {
        formatter.format_linked_program_host(image, 1).unwrap()
    } else {
        formatter.format_linked_program_standalone(image, false)
    };
    let expanded = MacroExpander::new().expand(
        &macros,
        MacroExpanderOptions {
            strip_comments: true,
            ..Default::default()
        },
    );
    assert!(expanded.errors.is_empty(), "{:?}", expanded.errors);
    let code = expanded
        .expanded
        .split_once("END_BFHOST")
        .map_or(expanded.expanded.as_str(), |(_, body)| body);
    let ops = parse(code);
    let cells =
        (usize::from(image.bank_size).max(40 + 4 * image.instructions.len() + 36) + 33) * LANES;
    // Plain exports retain the original zero origin and wrapping tape contract.
    // Host exports add an eight-cell guard and require strict pointer bounds.
    let mut state = State {
        tape: vec![0; cells],
        origin: if host { ORIGIN } else { 0 },
        trace: vec![],
        output: vec![],
    };
    let (mut pc, mut pointer, mut fuel) = (0, 0usize, 100_000_000u64);
    while pc < ops.len() {
        assert!(
            fuel != 0,
            "bounded BF execution exhausted fuel; trace tail: {:?}",
            &state.trace[state.trace.len().saturating_sub(8)..]
        );
        fuel -= 1;
        match ops[pc] {
            Op::Add(delta) => state.tape[pointer] = state.tape[pointer].wrapping_add(delta),
            Op::Move(delta) => {
                let next = pointer as isize + delta;
                pointer = if host {
                    usize::try_from(next).unwrap_or_else(|_| panic!("strict lower tape bound: pc={pc}, pointer={pointer}, delta={delta}, trace tail={:?}", &state.trace[state.trace.len().saturating_sub(8)..]))
                } else {
                    next.rem_euclid(cells as isize) as usize
                };
                assert!(
                    pointer < cells,
                    "strict upper tape bound: {pointer} >= {cells}"
                );
            }
            Op::Clear => state.tape[pointer] = 0,
            Op::Open(end) if state.tape[pointer] == 0 => pc = end,
            Op::Close(start) if state.tape[pointer] != 0 => pc = start,
            Op::Output => {
                assert!(!host, "host mode must not duplicate TTY output");
                state.output.push(state.tape[pointer] as u8);
            }
            Op::Hook(b'$') if host => {
                let bank = state.register(2);
                let offset = state.register(1);
                let flat = usize::from(bank) * usize::from(image.bank_size) + usize::from(offset);
                assert_eq!(
                    pointer,
                    ORIGIN + LANES * (40 + 4 * flat),
                    "physical instruction and PC/PCB disagree"
                );
                state.trace.push((bank, offset));
            }
            Op::Hook(b'!') if host => {
                assert_eq!(pointer % LANES, 4);
                if pointer == ORIGIN + 4 {
                    state.output.push(state.tape[pointer] as u8);
                }
            }
            Op::Hook(b'?') if host => {
                assert_eq!(pointer % LANES, 4);
            }
            _ => {}
        }
        pc += 1;
    }
    assert_eq!(state.tape[state.origin], 0, "CPU must halt");
    if host {
        assert!(
            state.tape[..ORIGIN].iter().all(|&cell| cell == 0),
            "guard must stay zero"
        );
    }
    state
}

fn instruction(opcode: Opcode, a: u16, b: u16, c: u16) -> Instruction {
    Instruction::new(opcode, a, b, c)
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

#[test]
fn non_power_two_banks_entry_calls_raw_boundary_return_and_fallthrough() {
    let options = AssemblerOptions {
        bank_size: 37,
        max_immediate: 65535,
        ..Default::default()
    };
    let object = RippleAssembler::new(options)
        .assemble(include_str!("../../../rbf/tests/fixtures/multibank.asm"))
        .unwrap();
    let image = Linker::new(37).link(vec![object]).unwrap();
    assert_eq!(image.instructions.len(), 156);
    assert_eq!(image.entry_point, 37 * 4);
    let state = run(&image, true);
    assert_eq!(state.output, b"Y");
    for (reg, value) in [(5, 0), (7, 2), (8, 3), (23, 0)] {
        assert_eq!(state.register(reg), value);
    }
    assert_eq!(state.trace[0], (1, 0));
    for pair in [(1, 36), (4, 0), (1, 37), (2, 0), (2, 36), (3, 0), (4, 4)] {
        assert!(
            state.trace.contains(&pair),
            "missing architectural boundary {pair:?}"
        );
    }
    let returned = state
        .trace
        .iter()
        .position(|&pair| pair == (1, 37))
        .unwrap();
    assert_eq!(&state.trace[returned..returned + 2], &[(1, 37), (2, 0)]);
    let plain = run(&image, false);
    assert_eq!(plain.output, state.output);
    for reg in 0..32 {
        assert_eq!(plain.register(reg), state.register(reg));
    }
}

#[test]
fn taken_branch_past_bank_end_retains_raw_pc_and_same_pcb() {
    let b = 37usize;
    let mut code = vec![instruction(Opcode::Nop, 0, 0, 0); 2 * b + 2];
    code[2 * b - 1] = instruction(Opcode::Beq, 0, 0, 2);
    code[2 * b] = instruction(Opcode::Halt, 0, 0, 0);
    code[2 * b + 1] = instruction(Opcode::Store, 1, 0, 0); // Output raw PC=38.
    let image = image(b as u16, 2 * b - 1, code);
    let state = run(&image, true);
    assert_eq!(state.trace, [(1, 36), (1, 38), (2, 0)]);
    assert_eq!(state.output, [38]);
    assert_eq!(run(&image, false).output, [38]);
}

#[test]
fn backward_branch_near_bank_start_uses_wrapping_signed_displacement() {
    let mut code = vec![instruction(Opcode::Nop, 0, 0, 0); 37];
    code.extend([
        instruction(Opcode::Li, 7, 2, 0),
        instruction(Opcode::Addi, 7, 7, u16::MAX),
        instruction(Opcode::Bne, 7, 0, u16::MAX),
        instruction(Opcode::Halt, 0, 0, 0),
    ]);
    let state = run(&image(37, 37, code), true);
    assert_eq!(
        state.trace,
        [(1, 0), (1, 1), (1, 2), (1, 1), (1, 2), (1, 3)]
    );
    assert_eq!(state.register(7), 0);
}

#[test]
fn cross_bank_jalr_snapshots_aliased_target_and_preserves_nonlinking_return() {
    for destination in [15, 16] {
        // Link aliases target bank, then target offset.
        let b = 37usize;
        let mut code = vec![instruction(Opcode::Halt, 0, 0, 0); 2 * b + 9];
        code[b + 3..b + 11].copy_from_slice(&[
            instruction(Opcode::Li, 15, 2, 0),
            instruction(Opcode::Li, 16, 5, 0),
            instruction(Opcode::Li, 3, 9, 0),
            instruction(Opcode::Li, 4, 7, 0),
            instruction(Opcode::Jalr, destination, 15, 16),
            instruction(Opcode::Store, 3, 0, 0),
            instruction(Opcode::Store, 4, 0, 0),
            instruction(Opcode::Halt, 0, 0, 0),
        ]);
        code[2 * b + 5..2 * b + 9].copy_from_slice(&[
            instruction(Opcode::Store, destination, 0, 0),
            instruction(Opcode::Store, 4, 0, 0),
            instruction(Opcode::Store, 3, 0, 0),
            instruction(Opcode::Jalr, 0, 4, destination),
        ]);
        let image = image(b as u16, b + 3, code);
        let state = run(&image, true);
        assert_eq!(state.output, [8, 1, 9, 9, 1]);
        assert_eq!(&state.trace[4..7], &[(1, 7), (2, 5), (2, 6)]);
        assert!(state.trace.contains(&(1, 8)));
        assert_eq!(state.register(3), 9);
        assert_eq!(state.register(4), 1);
        assert_eq!(run(&image, false).output, state.output);
    }
}

#[test]
fn jalr_link_to_rab_keeps_saved_bank_after_snapshotting_bank_operand() {
    let b = 37usize;
    let mut code = vec![instruction(Opcode::Halt, 0, 0, 0); 2 * b + 7];
    code[b + 3..b + 6].copy_from_slice(&[
        instruction(Opcode::Li, 4, 2, 0),
        instruction(Opcode::Li, 16, 5, 0),
        instruction(Opcode::Jalr, 4, 4, 16),
    ]);
    code[2 * b + 5] = instruction(Opcode::Store, 4, 0, 0);
    let state = run(&image(b as u16, b + 3, code), true);
    assert_eq!(state.output, [1]);
    assert_eq!(state.trace, [(1, 3), (1, 4), (1, 5), (2, 5), (2, 6)]);
}

#[test]
fn nested_calls_restore_nonzero_return_bank_before_returning_to_caller() {
    let b = 37usize;
    let mut code = vec![instruction(Opcode::Halt, 0, 0, 0); 2 * b + 7];
    code[..4].copy_from_slice(&[
        instruction(Opcode::Li, 7, 0, 0),
        instruction(Opcode::Jal, 3, 1, 5),
        instruction(Opcode::Store, 7, 0, 0),
        instruction(Opcode::Halt, 0, 0, 0),
    ]);
    code[b + 5..b + 14].copy_from_slice(&[
        instruction(Opcode::Add, 23, 3, 0), // Save outer RA/RAB in S0/S1.
        instruction(Opcode::Add, 24, 4, 0),
        instruction(Opcode::Addi, 7, 7, 1),
        instruction(Opcode::Jal, 3, 2, 3),
        instruction(Opcode::Xori, 15, 4, 1), // Inner return preserved RAB=1.
        instruction(Opcode::Store, 15, 0, 0),
        instruction(Opcode::Add, 4, 24, 0),
        instruction(Opcode::Add, 3, 23, 0),
        instruction(Opcode::Jalr, 0, 4, 3),
    ]);
    code[2 * b + 3..2 * b + 7].copy_from_slice(&[
        instruction(Opcode::Addi, 7, 7, 2),
        instruction(Opcode::Store, 4, 0, 0),
        instruction(Opcode::Store, 3, 0, 0),
        instruction(Opcode::Jalr, 0, 4, 3),
    ]);
    let image = image(b as u16, 0, code);
    let state = run(&image, true);
    assert_eq!(state.output, [1, 9, 0, 3]);
    for boundary in [(0, 1), (1, 5), (1, 8), (2, 3), (1, 9), (0, 2)] {
        assert!(state.trace.contains(&boundary));
    }
    assert_eq!(state.register(3), 2);
    assert_eq!(state.register(4), 0);
    let plain = run(&image, false);
    assert_eq!(plain.output, state.output);
    for register in 0..32 {
        assert_eq!(plain.register(register), state.register(register));
    }
}
