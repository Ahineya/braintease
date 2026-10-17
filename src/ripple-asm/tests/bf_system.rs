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

fn run(image: &LinkedProgram, host: bool, irq: bool) -> State {
    let formatter = MacroFormatter::new().with_irq(irq);
    let macros = if host {
        formatter.format_linked_program_host(image, 8).unwrap()
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
    let cells = ((if host { 8 } else { 16 } * usize::from(image.bank_size))
        .max(40 + 4 * image.instructions.len() + 36)
        .max(224)
        + 33)
        * LANES;
    // Plain exports retain the original zero origin and wrapping tape contract.
    // Host exports add an eight-cell guard and require strict pointer bounds.
    let mut state = State {
        tape: vec![0; cells],
        origin: if host { ORIGIN } else { 0 },
        trace: vec![],
        output: vec![],
    };
    let (mut pc, mut pointer, mut fuel) = (0, 0usize, 200_000_000u64);
    while pc < ops.len() {
        assert!(
            fuel != 0,
            "bounded BF execution exhausted fuel at BF op {pc}, pointer {pointer}, error={}, PC={}, state={}; trace tail: {:?}",
            state.tape[state.origin+57*8+1],state.register(1),state.tape[state.origin],
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
            Op::Open(end) => {
                let mut delta = 0isize;
                let mut changes = Vec::new();
                let mut safe = true;
                for op in &ops[pc + 1..end] {
                    match *op {
                        Op::Move(d) => {
                            delta += d;
                            if (pointer as isize + delta) < 0
                                || (pointer as isize + delta) >= cells as isize
                            {
                                safe = false;
                            }
                        }
                        Op::Add(v) => changes.push((delta, v)),
                        _ => {
                            safe = false;
                            break;
                        }
                    }
                }
                if safe
                    && delta == 0
                    && changes
                        .iter()
                        .filter(|(d, _)| *d == 0)
                        .map(|(_, v)| *v)
                        .fold(0u16, u16::wrapping_add)
                        == u16::MAX
                {
                    let n = state.tape[pointer];
                    for (d, v) in changes {
                        let p = (pointer as isize + d) as usize;
                        state.tape[p] = state.tape[p].wrapping_add(n.wrapping_mul(v));
                    }
                    pc = end;
                }
            }
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

fn assemble(source: &str) -> LinkedProgram {
    let options = AssemblerOptions {
        bank_size: 64,
        max_immediate: 65535,
        ..Default::default()
    };
    Linker::new(64)
        .link(vec![RippleAssembler::new(options)
            .assemble(source)
            .unwrap()])
        .unwrap()
}
fn field(s: &State, word: usize) -> u16 {
    s.tape[s.origin + word * 8 + 1]
}
fn both(source: &str, irq: bool, check: impl Fn(&State)) {
    let image = assemble(source);
    for host in [false, true] {
        let s = run(&image, host, irq);
        assert_eq!(field(&s, 57), 0, "CPU fault; trace={:?}", s.trace);
        for (word, cells) in s.tape[s.origin..].chunks_exact(8).enumerate() {
            assert_eq!(
                (cells[2], cells[5], cells[6]),
                (0, 0, 0),
                "scratch at word {word}"
            );
        }
        check(&s);
    }
}
#[test]
fn code_reads_snapshot_aliases_preserve_memory_and_all_four_cells() {
    for source in ["T0, T1", "X0, X1", "X2, X3"] {
        let (a, c) = source.split_once(", ").unwrap();
        let text=format!("start:\nLI {a}, 0\nLI {c}, 8\nLOADC R0, {a}, {c}\nHALT\nNOP\nNOP\nNOP\nNOP\nLI S3, 65535\n");
        both(&text, false, |s| {
            for (r, v) in [(11, 14), (12, 26), (13, 65535), (14, 0)] {
                assert_eq!(s.register(r), v);
            }
        });
    }
}
#[test]
fn code_store_changes_next_instruction_without_touching_data_or_x_registers() {
    both("start:\nLI T0, 0\nLI T1, 7\nLI X0, 270\nLI X1, 7\nLI X2, 65\nLI X3, 65535\nSTORC R0, T0, T1\nNOP\nSTORE A0, R0, R0\nHALT\n",false,|s| {
        assert_eq!(s.output,b"A"); assert_eq!(s.register(11),270); assert_eq!(s.register(14),65535);
        assert_eq!(s.tape[s.origin+8*(40+4*7)+3],14);
    });
}
#[test]
fn software_interrupt_resumes_and_preserves_call_registers() {
    both("start:\nLI RA, 33\nLI RAB, 44\nLI T0, 25\nLI T1, 14\nSTORE T1, R0, T0\nLI T0, 22\nLI T1, 1\nSTORE T1, R0, T0\nLI T0, 21\nSTORE T1, R0, T0\nLI A0, 65\nSTORE A0, R0, R0\nHALT\nNOP\nCSRR S0, 0\nLI T0, 23\nSTORE T1, R0, T0\nIRET\n",true,|s| {
        assert_eq!(s.output,b"A"); assert_eq!(s.register(3),33); assert_eq!(s.register(4),44); assert_eq!(s.register(23),10); assert_eq!(field(s,47),0);
    });
}
#[test]
fn irq_is_rejected_at_export_unless_enabled() {
    let image = assemble("start:\nCSRR T0, 11\nHALT\n");
    assert!(MacroFormatter::new()
        .format_linked_program_host(&image, 8)
        .unwrap_err()
        .contains("--bf-irq"));
    both("start:\nCSRR T0, 11\nHALT\n", true, |s| {
        assert_eq!(s.register(15), 2)
    });
}

#[test]
fn mmu_operations_are_rejected_instead_of_exporting_an_unprotected_cpu() {
    for source in ["start:\nSYSCALL\nHALT\n", "start:\nENTERU R0, T0, T1\nHALT\n", "start:\nCSRW T0, 13\nHALT\n"] {
        let image=assemble(source);
        let error=MacroFormatter::new().with_irq(true).format_linked_program_host(&image,8).unwrap_err();
        assert!(error.contains("does not support the MMU"),"{error}");
    }
}

#[test]
fn storc_self_modification_uses_snapshots_and_loadc_observes_written_words() {
    both("start:\nLI X0, 0\nLI X1, 4\nLI X2, 12\nLI X3, 65535\nSTORC R0, X0, X1\nLOADC R0, X0, X1\nHALT\n",false,|s| {
        assert_eq!((s.register(11),s.register(12),s.register(13),s.register(14)),(0,4,12,65535));
    });
    both("start:\nLI T0, 0\nLI T1, 6\nLI X0, 0\nLI X1, 65535\nLI X2, 32768\nLI X3, 123\nSTORC R0, T0, T1\nLI A0, 65\nHALT\n",false,|s| assert_eq!(s.register(7),65));
}
#[test]
fn new_code_bank_is_nop_initialized_then_executable() {
    both("start:\nLI T0, 1\nLI T1, 3\nLI X0, 14\nLI X1, 7\nLI X2, 65\nLI X3, 0\nSTORC R0, T0, T1\nLI T1, 4\nLI X0, 0\nSTORC R0, T0, T1\nLI T1, 2\nLOADC R0, T0, T1\nLI T1, 3\nJALR R0, T0, T1\n",false,|s| {
        assert_eq!(s.register(7),65); assert_eq!(s.register(11),0x42);
        assert_eq!(s.register(12),0); assert_eq!(s.register(13),0); assert_eq!(s.register(14),0);
    });
}
#[test]
fn raw_offsets_normalize_without_sixteen_bit_address_multiplication() {
    both("start:\nLI T0, 0\nLI T1, 65\nLI X0, 14\nLI X1, 26\nLI X2, 32768\nLI X3, 65535\nSTORC R0, T0, T1\nLI T0, 1\nLI T1, 1\nLOADC R0, T0, T1\nHALT\n",false,|s| {
        assert_eq!((s.register(11),s.register(12),s.register(13),s.register(14)),(14,26,32768,65535));
    });
}
#[test]
fn invalid_code_access_faults_before_touching_ram_or_registers() {
    for (op, bank, off, expected) in [
        ("LOADC", 0, 60, 7),
        ("LOADC", 65535, 65535, 6),
        ("STORC", 20, 0, 6),
    ] {
        let im = assemble(&format!(
            "start:\nLI T0, {bank}\nLI T1, {off}\n{op} R0, T0, T1\nHALT\n"
        ));
        for host in [false, true] {
            let s = run(&im, host, false);
            assert_eq!(field(&s, 57), expected);
            assert!(s.output.is_empty());
            for word in s.tape[s.origin..].chunks_exact(8) {
                assert_eq!(word[4], 0);
            }
        }
    }
}
#[test]
fn csr_errors_are_explicit_and_do_not_corrupt_the_cpu() {
    for (op, error) in [
        ("IRET", 5),
        ("CSRW T0, 11", 2),
        ("CSRW T0, 0", 3),
        ("CSRW R0, 5", 4),
        ("CSRR PC, 11", 1),
        ("CSRR PCB, 11", 1),
    ] {
        let im = assemble(&format!("start:\n{op}\nHALT\n"));
        for host in [false, true] {
            assert_eq!(field(&run(&im, host, true), 57), error, "{op}");
        }
    }
    // CSR 12 is now MMU_VERSION. Host exports reject the unsupported CSR
    // range (>=12) before execution; the plain CPU still checks unknown CSRs.
    let im = assemble("start:\nCSRR T0, 31\nHALT\n");
    assert_eq!(field(&run(&im, false, true), 57), 2);
    assert!(MacroFormatter::new().with_irq(true)
        .format_linked_program_host(&im, 8).unwrap_err().contains("does not support the MMU"));
}
#[test]
fn interrupt_stack_and_editable_saved_continuation_restore_atomically() {
    let text="start:\nLI SB, 2\nLI SP, 61\nLI FP, 49\nLI RA, 33\nLI RAB, 44\nLI T0, 3\nCSRW T0, 5\nLI T0, 1\nCSRW T0, 6\nLI T0, 25\nLI T1, 24\nSTORE T1, R0, T0\nLI T0, 22\nLI T1, 1\nSTORE T1, R0, T0\nLI T0, 21\nSTORE T1, R0, T0\nLI A0, 78\nHALT\nLI A0, 65\nHALT\nNOP\nNOP\nNOP\nMOVE S0, SB\nMOVE S1, SP\nMOVE S2, FP\nCSRR S3, 8\nLI T0, 19\nCSRW T0, 0\nLI T0, 0\nCSRW T0, 2\nLI T0, 23\nSTORE T1, R0, T0\nMOVE SB, R0\nMOVE SP, R0\nMOVE FP, R0\nIRET\n";
    both(text, true, |s| {
        for (r, v) in [
            (3, 33),
            (4, 44),
            (7, 65),
            (23, 3),
            (24, 1),
            (25, 1),
            (26, 61),
            (28, 2),
            (29, 61),
            (30, 49),
        ] {
            assert_eq!(s.register(r), v, "register {r}");
        }
        assert_eq!(field(s, 46), 0);
        assert_eq!(field(s, 47), 0);
    });
}
#[test]
fn priority_acknowledgement_reassertion_and_non_nesting() {
    let text="start:\nLI T0, 25\nLI T1, 12\nSTORE T1, R0, T0\nLI T0, 21\nLI T1, 5\nSTORE T1, R0, T0\nLI T0, 22\nSTORE T1, R0, T0\nLI A0, 65\nSTORE A0, R0, R0\nHALT\nNOP\nLI T0, 26\nLOAD A1, R0, T0\nSTORE A1, R0, R0\nLI T0, 23\nLI T1, 1\nSTORE T1, R0, T0\nLOAD A2, R0, T0\nIRET\n";
    both(text, true, |s| {
        assert_eq!(s.output, [1, 3, 65]);
        assert_eq!(s.register(9), 1);
        assert_eq!(field(s, 45), 0);
    });
}
#[test]
fn unacknowledged_source_is_requeued_and_pending_csr_clears_selected_bits() {
    let text="start:\nLI T0, 25\nLI T1, 13\nSTORE T1, R0, T0\nLI T0, 21\nLI T1, 5\nSTORE T1, R0, T0\nLI T0, 22\nLI T1, 1\nSTORE T1, R0, T0\nLI A0, 65\nHALT\nNOP\nNOP\nADDI S0, S0, 1\nLI T0, 2\nBNE S0, T0, again\nLI T0, 0\nCSRW T0, 2\nLI T0, 4\nCSRW T0, 10\nagain:\nIRET\n";
    both(text, true, |s| {
        assert_eq!(s.register(23), 2);
        assert_eq!(field(s, 45), 1);
        assert_eq!(s.register(7), 65);
    });
}
#[test]
fn timer_counts_guest_instructions_and_handler_work_independent_of_host_ticks() {
    let text="start:\nLI T0, 25\nLI T1, 18\nSTORE T1, R0, T0\nLI T0, 22\nLI T1, 2\nSTORE T1, R0, T0\nLI T0, 5\nCSRW T0, 3\nLI A0, 1\nADDI A0, A0, 1\nADDI A0, A0, 1\nADDI A0, A0, 1\nADDI A0, A0, 1\nLI A1, 65\nHALT\nNOP\nNOP\nNOP\nMOVE S0, A0\nLI T0, 0\nCSRW T0, 3\nCSRR S1, 10\nLI T0, 23\nSTORE T1, R0, T0\nIRET\n";
    both(text, true, |s| {
        assert_eq!(s.register(23), 5);
        assert_eq!(s.register(8), 65);
        assert_eq!(field(s, 54), 0);
    });
}
#[test]
fn compile_time_switch_removes_the_irq_path_entirely() {
    let image = assemble("start:\nNOP\nHALT\n");
    let default = MacroFormatter::new()
        .format_linked_program_host(&image, 8)
        .unwrap();
    let forced = MacroFormatter::new()
        .with_irq(true)
        .format_linked_program_host(&image, 8)
        .unwrap()
        .replace("#define ENABLE_IRQ 1", "#define ENABLE_IRQ 0");
    let expand = |s: &str| {
        let out = MacroExpander::new().expand(
            s,
            MacroExpanderOptions {
                strip_comments: true,
                ..Default::default()
            },
        );
        assert!(out.errors.is_empty(), "{:?}", out.errors);
        out.expanded
    };
    let a = expand(&default);
    let b = expand(&forced);
    assert_eq!(a, b);
    let on = expand(
        &MacroFormatter::new()
            .with_irq(true)
            .format_linked_program_host(&image, 8)
            .unwrap(),
    );
    assert!(on.len() > a.len());
}

#[test]
fn non_power_of_two_code_banks_and_interrupt_vector_banks_work() {
    let mut code = vec![instruction(Opcode::Halt, 0, 0, 0); 86];
    let start = [
        instruction(Opcode::Li, 15, 24, 0),
        instruction(Opcode::Li, 16, 2, 0),
        instruction(Opcode::Store, 16, 0, 15),
        instruction(Opcode::Li, 15, 25, 0),
        instruction(Opcode::Li, 16, 5, 0),
        instruction(Opcode::Store, 16, 0, 15),
        instruction(Opcode::Li, 15, 22, 0),
        instruction(Opcode::Li, 16, 1, 0),
        instruction(Opcode::Store, 16, 0, 15),
        instruction(Opcode::Li, 15, 21, 0),
        instruction(Opcode::Store, 16, 0, 15),
        instruction(Opcode::Li, 7, 65, 0),
        instruction(Opcode::Halt, 0, 0, 0),
    ];
    code[..start.len()].copy_from_slice(&start);
    code[79..86].copy_from_slice(&[
        instruction(Opcode::Csrr, 23, 0, 0),
        instruction(Opcode::Csrr, 24, 1, 0),
        instruction(Opcode::Loadc, 0, 2, 0), // Load code at bank=PCB=2, offset=R0=0.
        instruction(Opcode::Li, 15, 23, 0),
        instruction(Opcode::Store, 16, 0, 15),
        instruction(Opcode::Iret, 0, 0, 0),
        instruction(Opcode::Halt, 0, 0, 0),
    ]);
    let image = image(37, 0, code);
    for host in [false, true] {
        let s = run(&image, host, true);
        assert_eq!(field(&s, 57), 0);
        assert_eq!(s.register(7), 65);
        assert_eq!(s.register(23), 11);
        assert_eq!(s.register(24), 0);
        assert_eq!(s.register(11), 0); // Bank 2 offset 0 is HALT.
        if host {
            assert!(s.trace.contains(&(2, 5)));
        }
    }
}
#[test]
fn a_source_reasserted_before_ack_survives_and_zero_ack_does_not_ack() {
    let text="start:\nLI T0, 25\nLI T1, 13\nSTORE T1, R0, T0\nLI T0, 22\nLI T1, 1\nSTORE T1, R0, T0\nLI T0, 21\nSTORE T1, R0, T0\nLI A0, 65\nHALT\nNOP\nNOP\nNOP\nADDI S0, S0, 1\nLI T0, 1\nBNE S0, T0, ack\nLI T0, 21\nSTORE T1, R0, T0\nack:\nLI T0, 23\nSTORE T1, R0, T0\nLOAD S1, R0, T0\nIRET\n";
    both(text, true, |s| {
        assert_eq!(s.register(23), 2);
        assert_eq!(s.register(24), 1);
        assert_eq!(field(s, 45), 0);
        assert_eq!(s.register(7), 65);
    });
    let text="start:\nLI T0, 25\nLI T1, 11\nSTORE T1, R0, T0\nLI T0, 22\nLI T1, 1\nSTORE T1, R0, T0\nLI T0, 21\nSTORE T1, R0, T0\nHALT\nNOP\nNOP\nLI T0, 23\nSTORE R0, R0, T0\nCSRW R0, 2\nIRET\n";
    both(text, true, |s| assert_eq!(field(s, 45), 1));
}
#[test]
fn timer_high_word_borrows_and_configuration_does_not_count_itself() {
    both(
        "start:\nLI T0, 1\nCSRW T0, 4\nCSRR S0, 3\nCSRR S1, 4\nNOP\nHALT\n",
        true,
        |s| {
            assert_eq!(s.register(23), 0);
            assert_eq!(s.register(24), 1);
            assert_eq!(field(s, 52), 65532);
            assert_eq!(field(s, 53), 0);
            assert_eq!(field(s, 45), 0);
        },
    );
}
#[test]
fn high_irq_bits_and_write_one_clear_preserve_other_sources() {
    both("start:\nLI T0, 21\nLI T1, 32773\nSTORE T1, R0, T0\nLI T1, 4\nCSRW T1, 10\nCSRR S0, 10\nLI T1, 32768\nCSRW T1, 10\nCSRR S1, 10\nLI T1, 65535\nCSRW T1, 10\nCSRR S2, 10\nHALT\n",true,|s| {
        assert_eq!(s.register(23),32769);assert_eq!(s.register(24),1);assert_eq!(s.register(25),0);
    });
}

#[test]
fn a_halted_cpu_does_not_publish_a_timer_expiration() {
    both("start:\nLI T0, 1\nCSRW T0, 3\nHALT\n", true, |s| {
        assert_eq!(field(s, 45), 0)
    });
}

#[test]
fn rvm_assembly_examples_work_without_host_instruction_execution() {
    both(
        include_str!("../../../rvm/tests/asm/test_irq.asm"),
        true,
        |s| assert_eq!(s.output, b"AY\n"),
    );
    both(
        include_str!("../../../rvm/tests/asm/test_loadc_storc.asm"),
        false,
        |s| assert_eq!(s.output, b"AY\n"),
    );
}
#[test]
fn code_growth_and_word_transfers_cross_the_sixteen_bit_tape_word_boundary() {
    let mut image=assemble("start:\nLI T0, 1\nLI T1, 3\nLI X0, 14\nLI X1, 7\nLI X2, 65\nLI X3, 65535\nSTORC R0, T0, T1\nLOADC R0, T0, T1\nLI T1, 2\nLOADC R0, T0, T1\nHALT\n");
    image.bank_size = 16384;
    for host in [false, true] {
        let s = run(&image, host, false);
        assert_eq!(field(&s, 57), 0);
        assert_eq!(
            (
                s.register(11),
                s.register(12),
                s.register(13),
                s.register(14)
            ),
            (66, 0, 0, 0)
        );
        let opcode = 40 + 4 * (16384 + 3);
        for (word, expected) in [14, 7, 65, 65535].into_iter().enumerate() {
            assert_eq!(s.tape[s.origin + 8 * (opcode - word) + 3], expected);
        }
        for cells in s.tape[s.origin..].chunks_exact(8) {
            assert_eq!((cells[2], cells[5], cells[6]), (0, 0, 0));
            assert_eq!(cells[4], 0, "code writes must preserve RAM");
        }
    }
}

#[test]
fn memory_routes_cross_65535_words_and_preserve_overlapping_code() {
    let mut im=assemble("start:\nLI T0, 2\nLI T1, 3\nLI A0, 65535\nSTORE A0, T0, T1\nLOAD S0, T0, T1\nLOAD T1, T0, T1\nHALT\n");
    im.bank_size = 32768;
    for host in [false, true] {
        let s = run(&im, host, false);
        assert_eq!(s.register(23), 65535);
        assert_eq!(s.register(16), 65535);
        for (word, cells) in s.tape[s.origin..].chunks_exact(8).enumerate() {
            assert_eq!(
                cells[4],
                if word == 65539 { 65535 } else { 0 },
                "RAM {word}"
            );
            assert_eq!((cells[2], cells[5], cells[6]), (0, 0, 0), "scratch {word}");
        }
    }
}
