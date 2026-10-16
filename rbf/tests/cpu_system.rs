//! Differential execution: the real BF CPU (interpreter and JIT) against RVM.
use bf_wasm::engine::Program;
use rbf::{
    devices::BfDevices,
    image::{Layout, ORIGIN},
    runner::{Host, RunOptions},
};
use ripple_asm::{AssemblerOptions, Linker, MacroFormatter, RippleAssembler};
use rvm::{vm::VMState, VM};
use std::time::Duration;

fn execute(source: &str, irq: bool) {
    execute_banked(source, irq, 64);
}
fn execute_banked(source: &str, irq: bool, bank: u16) {
    let obj = RippleAssembler::new(AssemblerOptions {
        bank_size: bank,
        max_immediate: 65535,
        ..Default::default()
    })
    .assemble(source)
    .unwrap();
    let image = Linker::new(bank).link(vec![obj]).unwrap();
    let mut bytes = Vec::new();
    MacroFormatter::new()
        .with_irq(irq)
        .write_linked_program_host_bf(&image, 8, &mut bytes)
        .unwrap();
    let source = String::from_utf8(bytes).unwrap();
    let (layout, body) = Layout::parse(&source, 100000).unwrap_or_else(|e| {
        let body = source.split_once("END_BFHOST").unwrap().1;
        panic!(
            "{e}: {:?}",
            body.chars()
                .filter(|c| !c.is_whitespace() && !"+-<>[],.?!$".contains(*c))
                .take(100)
                .collect::<String>()
        )
    });
    let program = Program::parse(body, true, true).unwrap();
    let mut reference = VM::with_memory_size(bank, usize::from(bank) * 8);
    reference.host_input_enabled = false;
    reference.load_binary(&image.to_binary()).unwrap();
    for _ in 0..10000 {
        if matches!(reference.state, VMState::Halted) {
            break;
        }
        reference.step().unwrap();
    }
    assert!(
        matches!(reference.state, VMState::Halted),
        "state={:?}, regs={:?}, count={}",
        reference.state,
        reference.registers,
        reference.executed_instructions
    );
    let mut expected = None;
    for jit in [false, true] {
        let mut host =
            Host::new(layout.clone(), BfDevices::new(bank, 35, 1, None).unwrap()).unwrap();
        let mut out = Vec::new();
        let options = RunOptions {
            jit,
            timeout: Some(Duration::from_secs(60)),
            max_dispatches: Some(500_000_000),
            max_instructions: Some(10000),
            ..Default::default()
        };
        assert_eq!(host.run(&program, &options, &mut out).unwrap(), "halted");
        for r in 0..32 {
            assert_eq!(
                host.machine.read_cell(ORIGIN + 8 * (r + 1) + 1),
                reference.registers[r],
                "register {r}, jit={jit}"
            );
        }
        let extent = usize::from(host.machine.read_cell(ORIGIN + 8 * 81 + 1)) * usize::from(bank)
            + usize::from(host.machine.read_cell(ORIGIN + 8 * 82 + 1));
        for (index, expected) in reference.instructions.iter().take(extent).enumerate() {
            for (word, value) in [
                expected.opcode as u16,
                expected.word1,
                expected.word2,
                expected.word3,
            ]
            .into_iter()
            .enumerate()
            {
                assert_eq!(
                    host.machine
                        .read_cell(ORIGIN + 8 * (40 + 4 * index - word) + 3),
                    value,
                    "IMEM {index} field {word}, jit={jit}"
                );
            }
        }
        for a in (21..27).chain(32..usize::from(bank) * 8) {
            assert_eq!(
                host.machine.read_cell(ORIGIN + 8 * a + 4),
                reference.memory[a],
                "RAM {a}, jit={jit}"
            );
        }
        if reference.executed_instructions > 14 {
            // Pause with live code/trap state, then resume the same BF machine.
            // This also checks that host run boundaries neither re-deliver nor
            // lose IRQs and do not invalidate dynamically written instructions.
            let mut resumed =
                Host::new(layout.clone(), BfDevices::new(bank, 35, 1, None).unwrap()).unwrap();
            let mut resumed_output = Vec::new();
            let pause = RunOptions {
                max_instructions: Some(14),
                ..options
            };
            let error = resumed
                .run(&program, &pause, &mut resumed_output)
                .unwrap_err();
            assert!(error.contains("instruction limit"), "{error}");
            let resume = RunOptions {
                max_instructions: Some(10000),
                ..pause
            };
            assert_eq!(
                resumed.run(&program, &resume, &mut resumed_output).unwrap(),
                "halted"
            );
            assert_eq!(resumed.machine.tape(), host.machine.tape());
            assert_eq!(resumed_output, out);
        }
        let state = (host.machine.tape().to_vec(), out);
        if let Some(expected) = &expected {
            assert_eq!(&state, expected);
        } else {
            expected = Some(state);
        }
    }
}
#[test]
fn instructions_irq_timer_and_code_growth_match_rvm() {
    execute("start:\nLI T0, 1\nCSRW T0, 3\nHALT\n", true);
    execute("start:\nLI T0, 1\nLI T1, 3\nLI X0, 14\nLI X1, 7\nLI X2, 65\nLI X3, 0\nSTORC R0, T0, T1\nLI T1, 4\nLI X0, 0\nSTORC R0, T0, T1\nLI T1, 2\nLOADC R0, T0, T1\nLI T1, 3\nJALR R0, T0, T1\n",false);
    execute("start:\nLI T0, 25\nLI T1, 18\nSTORE T1, R0, T0\nLI T0, 22\nLI T1, 2\nSTORE T1, R0, T0\nLI T0, 5\nCSRW T0, 3\nLI A0, 1\nADDI A0, A0, 1\nADDI A0, A0, 1\nADDI A0, A0, 1\nADDI A0, A0, 1\nLI A1, 65\nHALT\nNOP\nNOP\nNOP\nMOVE S0, A0\nLI T0, 0\nCSRW T0, 3\nCSRR S1, 10\nLI T0, 23\nSTORE T1, R0, T0\nIRET\n",true);
}
#[test]
fn invalid_runtime_state_reports_fault_in_both_bf_engines() {
    for text in [
        "start:\nIRET\nHALT\n",
        "start:\nCSRW R0, 0\nHALT\n",
        "start:\nLI T0, 65535\nLOADC R0, T0, T0\nHALT\n",
    ] {
        let obj = RippleAssembler::new(AssemblerOptions::default())
            .assemble(text)
            .unwrap();
        let image = Linker::new(64).link(vec![obj]).unwrap();
        let mut bytes = Vec::new();
        MacroFormatter::new()
            .with_irq(true)
            .write_linked_program_host_bf(&image, 8, &mut bytes)
            .unwrap();
        let source = String::from_utf8(bytes).unwrap();
        let (layout, body) = Layout::parse(&source, 100000).unwrap_or_else(|e| {
            let body = source.split_once("END_BFHOST").unwrap().1;
            panic!(
                "{e}: {:?}",
                body.chars()
                    .filter(|c| !c.is_whitespace() && !"+-<>[],.?!$".contains(*c))
                    .take(100)
                    .collect::<String>()
            )
        });
        let program = Program::parse(body, true, true).unwrap();
        for jit in [false, true] {
            let mut host =
                Host::new(layout.clone(), BfDevices::new(64, 35, 1, None).unwrap()).unwrap();
            let error = host
                .run(
                    &program,
                    &RunOptions {
                        jit,
                        timeout: Some(Duration::from_secs(30)),
                        ..Default::default()
                    },
                    &mut Vec::new(),
                )
                .unwrap_err();
            assert!(error.contains("BF CPU execution fault"), "{error}");
        }
    }
}

#[test]
fn arithmetic_high_bits_zero_divisors_and_masked_shifts_match_rvm() {
    let mut source = String::from("start:\nLI GP, 3\nLI FP, 32\n");
    // Results go to independent RAM cells so every intermediate answer is
    // compared, including rd=rs/rt and repeated reads of the same source.
    for (lhs, rhs) in [
        (0, 0),
        (65535, 0),
        (65535, 1),
        (65535, 32768),
        (32768, 65535),
        (32769, 32768),
        (32767, 17),
        (0xaaaa, 0x5555),
    ] {
        for op in ["AND", "OR", "XOR", "DIV", "MOD", "SLL", "SRL"] {
            source.push_str(&format!(
                "LI T0, {lhs}\nLI T1, {rhs}\n{op} T0, T0, T1\nSTORE T0, GP, FP\nADDI FP, FP, 1\n"
            ));
        }
    }
    for count in [0, 1, 15, 16, 17, 31, 32, 255, 32768, 65535] {
        for op in ["SLLI", "SRLI"] {
            source.push_str(&format!(
                "LI T0, 32769\n{op} T0, T0, {count}\nSTORE T0, GP, FP\nADDI FP, FP, 1\n"
            ));
        }
    }
    source.push_str("LI T0, 65535\nDIVI T1, T0, 0\nMODI T2, T0, 0\nDIVI T3, T0, 32768\nMODI T4, T0, 32768\nHALT\n");
    execute_banked(&source, false, 512);
}

#[test]
fn every_register_and_memory_alias_route_matches_rvm() {
    let mut source = String::from("start:\n");
    for r in 3..32 {
        source.push_str(&format!("LI R{r}, {}\n", (r * 3497 + 32768) & 65535));
    }
    for r in 0..32 {
        source.push_str(&format!(
            "MOVE X0, R{r}\nLI X1, {}\nLI X2, 3\nSTORE X0, X2, X1\n",
            32 + r
        ));
    }
    source.push_str("LI T0, 2\nLI A0, 65535\nSTORE A0, T0, T0\nLOAD T0, T0, T0\nLI T0, 1\nLI T1, 65\nSTORE A0, T0, T1\nLOAD T1, T0, T1\nLOAD R0, T0, T0\nHALT\n");
    execute_banked(&source, false, 512);
}
