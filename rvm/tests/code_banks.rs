use ripple_asm::{AssemblerOptions, LinkedProgram, Linker, Opcode, RelocKind, RippleAssembler};
use rvm::{vm::VMState, Instr, VM};

fn image(source: &str, bank: u16) -> LinkedProgram {
    let assembler = RippleAssembler::new(AssemblerOptions {
        bank_size: bank,
        ..Default::default()
    });
    Linker::new(bank)
        .link(vec![assembler.assemble(source).unwrap()])
        .unwrap()
}
fn run(image: &LinkedProgram, jit: bool, base: u16) -> [u16; 32] {
    execute(image, jit, base).0
}
fn execute(image: &LinkedProgram, jit: bool, base: u16) -> ([u16; 32], u64) {
    let mut vm = VM::with_options(image.bank_size, usize::from(image.bank_size) * 4, None);
    vm.host_input_enabled = false;
    vm.jit_enabled = jit;
    let offset = usize::from(base) * usize::from(image.bank_size);
    vm.instructions = vec![Instr::new(0, 0, 0, 0, 0); offset];
    vm.instructions.extend(
        image
            .instructions
            .iter()
            .map(|i| Instr::new(i.opcode, i.word0, i.word1, i.word2, i.word3)),
    );
    vm.registers[1] = (image.entry_insn() % u32::from(image.bank_size)) as u16;
    vm.registers[2] = base + (image.entry_insn() / u32::from(image.bank_size)) as u16;
    vm.state = VMState::Running;
    for _ in 0..100 {
        if !matches!(vm.state, VMState::Running) {
            break;
        }
        vm.run_batch(4096).unwrap();
    }
    assert!(matches!(vm.state, VMState::Halted), "program did not halt");
    (
        vm.registers,
        vm.jit_stats().map_or(0, |s| s.compiled_regions as u64),
    )
}

#[test]
fn every_comparison_takes_both_paths_at_every_bank_alignment() {
    for bank in [3, 8, 37] {
        for (op, yes, no) in [("BEQ", 1, 2), ("BNE", 2, 1), ("BLT", -1, 1), ("BGE", 1, -1)] {
            for prefix in 0..bank {
                for (value, expected) in [(yes, 42), (no, 7)] {
                    let source = format!("start:\nLI A0, {value}\nLI A1, 1\n{}L_site:\n{op} A0, A1, L_far\nLI RV0, 7\nBEQ R0, R0, L_end\n{}L_far:\nLI RV0, 42\nL_end:\nHALT\n", "NOP\n".repeat(prefix as usize), "NOP\n".repeat(bank as usize * 2));
                    let program = image(&source, bank);
                    let registers = run(&program, false, 0);
                    assert_eq!(
                        registers[5], expected,
                        "{op} {value}, bank={bank}, prefix={prefix}"
                    );
                    assert_eq!((registers[3], registers[4]), (0, 0));
                }
            }
        }
    }
}

#[test]
fn backward_far_branches_nested_calls_and_link_registers_match_on_jit() {
    let source = include_str!("../../src/ripple-asm/tests/fixtures/bank-spanning-function.asm")
        .replace("LI S0, 2", "LI S0, 4096")
        .replace("XORI T1, S1, 8", "XORI T1, S1, 16384");
    for jit in [false, true] {
        let (registers, compiled) = execute(&image(&source, 37), jit, 0);
        assert_eq!((registers[23], registers[24]), (0, 16384));
        assert_eq!((registers[3], registers[4]), (777, 888));
        assert_eq!(registers[7], 89);
        if jit {
            assert!(
                compiled > 0,
                "the hot loop must exercise native JIT regions"
            );
        }
    }
}

#[test]
fn direct_and_indirect_calls_at_bank_ends_return_once() {
    for bank in [8, 37, 65535] {
        for indirect in [false, true] {
            let setup = if indirect {
                "LI T0, %bank(callee)\nLI T1, %offset(callee)\n"
            } else {
                ""
            };
            let call = if indirect {
                "JALR RA, T0, T1"
            } else {
                "CALL callee"
            };
            let prefix = usize::from(bank) - 2 - if indirect { 2 } else { 0 };
            let source = format!("start:\nLI RV0, 0\n{setup}{}{call}\nADDI RV0, RV0, 1\n{}HALT\ncallee:\nADDI RV0, RV0, 2\nRET\n", "NOP\n".repeat(prefix), "NOP\n".repeat(4));
            let program = image(&source, bank);
            assert_eq!(
                run(&program, false, 0)[5],
                3,
                "bank={bank}, indirect={indirect}"
            );
        }
    }
}

#[test]
fn far_jumps_and_function_pointer_components_relocate_together() {
    let mut program = image("start:\nLI T0, %bank(callee)\nLI T1, %offset(callee)\nJALR RA, T0, T1\nBEQ R0, R0, L_end\nNOP\nNOP\nNOP\nNOP\nNOP\nNOP\nNOP\nNOP\nNOP\nNOP\nL_end:\nHALT\ncallee:\nLI RV0, 42\nRET\n", 8);
    assert!(program.relocs.iter().any(|r| r.kind == RelocKind::JalAbs
        && program.instructions[r.index as usize].opcode == Opcode::Jal as u8));
    program.apply_code_bank_base(7);
    for jit in [false, true] {
        assert_eq!(run(&program, jit, 7)[5], 42);
    }
    // Both executable encodings carry the generated far-jump relocations.
    assert_eq!(&program.to_sys(7, 1, 2)[..4], b"SYS1");
    let rxe = program.to_rxe();
    assert_eq!(
        u32::from_le_bytes(rxe[20..24].try_into().unwrap()) as usize,
        program.relocs.len()
    );
}

#[test]
fn a_seventy_thousand_instruction_function_runs_in_both_directions() {
    // Near displacements beyond signed i16 still work by architectural u16
    // wrapping. Only crossing a bank requires a far jump.
    for (padding, bank) in [(40000, 64000), (65520, 65535), (70000, 64000)] {
        let source = format!("start:\nLI A0, 2\nL_head:\nADDI A0, A0, -1\nBEQ A0, R0, L_end\n{}BNE A0, R0, L_head\nL_end:\nLI RV0, 42\nHALT\n", "NOP\n".repeat(padding));
        let program = image(&source, bank);
        if padding < bank as usize {
            assert_eq!(program.instructions[2].opcode, Opcode::Beq as u8);
        }
        assert_eq!(run(&program, false, 0)[5], 42);
    }
}
