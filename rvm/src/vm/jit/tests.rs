use super::*;
use crate::vm::VMState;

struct Fixture {
    vm: VM,
    path: std::path::PathBuf,
}

impl Fixture {
    fn new(bank: u16) -> Self {
        use std::sync::atomic::{AtomicU64, Ordering};
        static ID: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!(
            "rvm-native-{}-{}.disk",
            std::process::id(),
            ID.fetch_add(1, Ordering::Relaxed)
        ));
        Self {
            vm: VM::with_options(bank, 8192, Some(path.clone())),
            path,
        }
    }
}

impl std::ops::Deref for Fixture {
    type Target = VM;
    fn deref(&self) -> &VM {
        &self.vm
    }
}

impl std::ops::DerefMut for Fixture {
    fn deref_mut(&mut self) -> &mut VM {
        &mut self.vm
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

fn i(op: u8, a: u16, b: u16, c: u16) -> Instr {
    Instr::new(op, op, a, b, c)
}

/// Force native translation so short correctness fixtures test the emitter,
/// independently of the production warmup policy.
fn compare(bank: u16, code: Vec<Instr>) {
    for budget in [1, 7, 64, 65_536] {
        let mut reference = Fixture::new(bank);
        let mut native = Fixture::new(bank);
        for vm in [&mut reference, &mut native] {
            vm.host_input_enabled = false;
            vm.instructions = code.clone();
            vm.state = VMState::Running;
            vm.registers[0] = 123;
        }
        native.jit_enabled = true;
        let mut engine = JitEngine::new().unwrap();
        engine.ensure_table(code.len());
        engine.hits.fill(HOT_THRESHOLD);
        native.jit = Some(engine);
        for step in 0..100_000 {
            let expected = reference.run_batch(budget);
            let actual = native.run_batch(budget);
            assert_eq!(actual, expected, "batch {step}, budget {budget}");
            assert_eq!(native.registers, reference.registers, "budget {budget}");
            assert_eq!(native.memory, reference.memory);
            assert_eq!(
                native.executed_instructions,
                reference.executed_instructions
            );
            assert_eq!(native.output_buffer, reference.output_buffer);
            assert_eq!(native.output_ready, reference.output_ready);
            assert_eq!(native.device_error, reference.device_error);
            assert_eq!(
                format!("{:?}", native.state),
                format!("{:?}", reference.state)
            );
            assert_eq!(
                native.jit_depth, 0,
                "native frames must unwind at every batch boundary"
            );
            let words = |vm: &VM| {
                vm.instructions
                    .iter()
                    .map(|i| (i.opcode, i.word1, i.word2, i.word3))
                    .collect::<Vec<_>>()
            };
            assert_eq!(words(&native), words(&reference));
            if actual.is_err() || !matches!(native.state, VMState::Running) {
                assert!(native.jit_stats().unwrap().compiled_regions > 0);
                break;
            }
            assert!(step < 99_999, "fixture did not terminate");
        }
    }
}

#[test]
fn native_alu_and_ram_match_at_every_budget_boundary() {
    let mut code = vec![i(0x0E, 15, 65535, 0), i(0x0E, 16, 1, 0)];
    let mut random = 0x12345678u32;
    for _ in 0..300 {
        random = random.wrapping_mul(1664525).wrapping_add(1013904223);
        let op = [1, 2, 3, 4, 5, 6, 7, 8, 9, 0x1A, 0x1B, 0x1C][random as usize % 12];
        code.push(i(
            op,
            15 + ((random >> 8) % 8) as u16,
            ((random >> 16) % 40) as u16,
            ((random >> 24) % 40) as u16,
        ));
    }
    code.extend([
        i(0x0E, 15, 1, 0),
        i(0x0E, 16, 400, 0),
        i(0x12, 17, 15, 16),
        i(0x11, 18, 15, 16),
        i(0, 0, 0, 0),
    ]);
    compare(64, code);
}

#[test]
fn native_pc_writes_and_noncanonical_targets_match() {
    let mut code = vec![i(0x13, 3, 0, 12); 16];
    code[8] = i(0, 0, 0, 0);
    code[12] = i(1, 15, 1, 0); // PC=12, PCB=0, not the normalized PC=4, PCB=1.
    compare(8, code.clone());
    code[0] = i(0x15, 0, 0, 12);
    compare(8, code);
    let mut code = vec![i(0x42, 0, 0, 0); 24];
    code[0] = i(0x0E, 1, 3, 0); // LI PC, 3; next fetch is 4.
    code[4] = i(0x0E, 2, 1, 0); // LI PCB, 1; next fetch is 21.
    code[21] = i(0x0E, 15, 7, 0);
    code[22] = i(0, 0, 0, 0);
    compare(16, code);
}

#[test]
fn nonlinking_jump_loops_do_not_allocate_native_frames() {
    compare(
        64,
        vec![
            i(0x0E, 15, 20000, 0),
            i(0x0A, 15, 15, 65535),
            i(0x15, 15, 0, 2),
            i(0x13, 0, 0, 1),
            i(0, 0, 0, 0),
        ],
    );
}

#[test]
fn guest_recursion_exceeding_native_depth_uses_the_driver() {
    // A chain of linked calls needs no guest stack for this fixture: the base
    // case halts. It exceeds the native depth cap repeatedly within one batch.
    compare(
        64,
        vec![
            i(0x0E, 15, 2000, 0),
            i(0x13, 3, 0, 3),
            i(0, 0, 0, 0),
            i(0x0A, 15, 15, 65535),
            i(0x15, 15, 0, 2),
            i(0x13, 3, 0, 3),
            i(0, 0, 0, 0),
        ],
    );
}

#[test]
fn callee_can_patch_its_callers_continuation() {
    compare(
        64,
        vec![
            i(0x13, 3, 0, 4),
            i(0x0E, 15, 1, 0),
            i(0, 0, 0, 0),
            i(0x42, 0, 0, 0),
            i(0x0E, 11, 0x0E, 0),
            i(0x0E, 12, 15, 0),
            i(0x0E, 13, 7, 0),
            i(0x0E, 14, 0, 0),
            i(0x0E, 16, 1, 0),
            i(0x21, 0, 0, 16),
            i(0x14, 0, 4, 3),
        ],
    );
}

#[test]
fn reload_clears_native_call_targets_from_the_previous_image() {
    let mut vm = Fixture::new(64);
    vm.instructions = vec![i(0x15, 0, 0, 1), i(0, 0, 0, 0)];
    vm.jit = Some(JitEngine::new().unwrap());
    let region = build_region(64, 0, &vm.instructions, &[]).unwrap();
    vm.jit.as_mut().unwrap().compile(64, 0, &region, 2).unwrap();
    assert!(vm.jit.as_ref().unwrap().get(1).is_some());
    vm.jit_flush();
    assert!(vm.jit.as_ref().unwrap().get(0).is_none());
    assert!(vm.jit.as_ref().unwrap().get(1).is_none());
}

#[test]
fn native_faults_and_tty_status_match() {
    compare(
        64,
        vec![
            i(0x0E, 15, b'A' as u16, 0),
            i(0x12, 15, 0, 0),
            i(0x0E, 16, 1, 0),
            i(0x11, 17, 0, 16),
            i(0, 0, 0, 0),
        ],
    );
    compare(
        64,
        vec![i(0x0E, 15, 128, 0), i(0x11, 16, 15, 0), i(0, 0, 0, 0)],
    );
    compare(
        64,
        vec![i(0x0E, 15, 128, 0), i(0x12, 16, 15, 0), i(0, 0, 0, 0)],
    );
    compare(64, vec![i(0x7F, 0, 0, 0)]);
}

#[test]
fn returning_to_a_noncanonical_link_address_matches() {
    let mut code = vec![i(0x42, 0, 0, 0); 16];
    code[0] = i(0x13, 0, 0, 3);
    code[3] = i(0x13, 3, 1, 2); // Link is PC=4, PCB=0 with a bank size of 4.
    code[4] = i(1, 15, 1, 0); // Must observe PC=4, not PC=0, PCB=1.
    code[5] = i(0, 0, 0, 0); // Stop after the normalized fetch at index 4.
    code[6] = i(0x14, 0, 4, 3);
    compare(4, code);
}

#[test]
fn native_memory_pointer_is_refreshed_between_batches() {
    let mut vm = Fixture::new(64);
    vm.instructions = vec![
        i(0x0E, 15, 1, 0),
        i(0x15, 0, 0, 1),
        i(0x11, 16, 15, 0),
        i(0, 0, 0, 0),
    ];
    vm.state = VMState::Running;
    vm.jit_enabled = true;
    let mut engine = JitEngine::new().unwrap();
    engine.ensure_table(4);
    engine.hits.fill(HOT_THRESHOLD);
    vm.jit = Some(engine);
    assert_eq!(vm.run_batch(2).unwrap(), 2);
    vm.memory = vec![0; 16384];
    vm.memory[64] = 1234;
    vm.run_batch(64).unwrap();
    assert_eq!(vm.registers[16], 1234);
}

#[test]
fn compiled_interiors_do_not_trigger_overlapping_translation() {
    let mut vm = Fixture::new(64);
    let mut code = vec![i(0x0E, 15, 3000, 0)];
    code.extend(std::iter::repeat_n(i(0x42, 0, 0, 0), 20));
    code.extend([
        i(0x0A, 15, 15, 65535),
        i(0x16, 15, 0, (-21i16) as u16),
        i(0, 0, 0, 0),
    ]);
    vm.instructions = code;
    vm.state = VMState::Running;
    vm.jit_enabled = true;
    let mut engine = JitEngine::new().unwrap();
    engine.ensure_table(vm.instructions.len());
    engine.hits.fill(HOT_THRESHOLD);
    vm.jit = Some(engine);
    while matches!(vm.state, VMState::Running) {
        assert!(vm.run_batch(7).unwrap() <= 7);
    }
    assert_eq!(vm.jit_stats().unwrap().compiled_regions, 1);
}

#[test]
fn native_irq_dispatch_and_code_module_reclamation_match() {
    use crate::constants::{HDR_IRQ_ENABLE, HDR_IRQ_STATUS, HDR_IRQ_VECTOR_OFF};
    compare(
        64,
        vec![
            i(0x0E, 15, HDR_IRQ_VECTOR_OFF as u16, 0),
            i(0x0E, 7, 12, 0),
            i(0x12, 7, 0, 15),
            i(0x0E, 15, HDR_IRQ_ENABLE as u16, 0),
            i(0x0E, 7, 1, 0),
            i(0x12, 7, 0, 15),
            i(0x0E, 15, HDR_IRQ_STATUS as u16, 0),
            i(0x12, 7, 0, 15),
            i(0x42, 0, 0, 0),
            i(0, 0, 0, 0),
            i(0x42, 0, 0, 0),
            i(0x42, 0, 0, 0),
            i(0x0E, 5, 99, 0),
            i(0, 0, 0, 0),
        ],
    );
    let mut code = vec![
        i(0x0E, 23, 40, 0),
        i(0x0E, 11, 0x0E, 0),
        i(0x0E, 12, 15, 0),
        i(1, 13, 23, 0),
        i(0x0E, 14, 0, 0),
        i(0x0E, 16, 20, 0),
        i(0x21, 0, 0, 16),
        i(0x13, 3, 0, 20),
        i(0x0A, 23, 23, 65535),
        i(0x16, 23, 0, (-6i16) as u16),
        i(0, 0, 0, 0),
    ];
    code.resize(20, i(0x42, 0, 0, 0));
    code.extend([i(0x0E, 15, 0, 0), i(0x14, 0, 4, 3)]);
    compare(64, code);
}

#[test]
fn native_cpu_control_and_interrupt_returns_use_precise_reference_exits() {
    // Each control operation is forced into a compiled region, including one
    // that edits the saved continuation and an IRET after an acknowledgement.
    let mut code = vec![i(0x42, 0, 0, 0); 32];
    code[..12].copy_from_slice(&[
        i(0x0e, 15, 25, 0), i(0x0e, 16, 20, 0), i(0x12, 16, 0, 15),
        i(0x0e, 15, 22, 0), i(0x0e, 16, 1, 0), i(0x12, 16, 0, 15),
        i(0x0e, 3, 1234, 0), i(0x0e, 4, 5678, 0),
        i(0x0e, 15, 21, 0), i(0x12, 16, 0, 15),
        i(0x0e, 5, 42, 0), i(0, 0, 0, 0),
    ]);
    code[20..26].copy_from_slice(&[
        i(0x23, 17, crate::constants::CSR_EPC, 0),
        i(0x24, 17, crate::constants::CSR_EPC, 0),
        i(0x23, 18, crate::constants::CSR_IRQ_VERSION, 0),
        i(0x0e, 15, 23, 0), i(0x12, 16, 0, 15), i(0x22, 0, 0, 0),
    ]);
    compare(64, code);
    compare(64, vec![i(0x22, 0, 0, 0)]);
    compare(64, vec![i(0x23, 15, 65535, 0)]);
}

#[test]
fn an_out_of_bounds_call_preserves_the_interpreter_diagnostic() {
    // A high bank can produce a linear address above i32::MAX. It must reach
    // the ordinary PC diagnostic rather than fail during native translation.
    compare(64000, vec![i(0x13, 3, 65535, 1), i(0, 0, 0, 0)]);
}
