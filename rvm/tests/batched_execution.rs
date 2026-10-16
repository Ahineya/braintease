//! Differential execution and host-boundary tests for the bounded interpreter.
use rvm::{
    devices::InputScript,
    runner::{self, ControlCommand, RunOptions},
    vm::VMState,
    Instr, VM,
};
use std::{
    fs,
    path::PathBuf,
    sync::{
        atomic::{AtomicU64, Ordering},
        mpsc,
    },
    time::Duration,
};

static ID: AtomicU64 = AtomicU64::new(0);
struct Fixture {
    vm: VM,
    path: PathBuf,
}
impl Fixture {
    fn new(bank: u16, code: Vec<Instr>) -> Self {
        let path = std::env::temp_dir().join(format!(
            "rvm-batch-{}-{}.img",
            std::process::id(),
            ID.fetch_add(1, Ordering::Relaxed)
        ));
        let mut vm = VM::with_options(bank, 256000, Some(path.clone()));
        vm.state = VMState::Running;
        vm.host_input_enabled = false;
        vm.instructions = code;
        Self { vm, path }
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
    }
}
fn i(op: u8, a: u16, b: u16, c: u16) -> Instr {
    Instr::new(op, op, a, b, c)
}
fn same(a: &VM, b: &VM) {
    assert_eq!(a.registers, b.registers);
    assert_eq!(a.memory, b.memory);
    let code = |vm: &VM| {
        vm.instructions
            .iter()
            .map(|i| (i.opcode, i.word0, i.word1, i.word2, i.word3))
            .collect::<Vec<_>>()
    };
    assert_eq!(code(a), code(b));
    assert_eq!(format!("{:?}", a.state), format!("{:?}", b.state));
    assert_eq!(a.executed_instructions, b.executed_instructions);
    assert_eq!(a.output_buffer, b.output_buffer);
    assert_eq!(a.input_buffer, b.input_buffer);
    assert_eq!(a.devices.tick(), b.devices.tick());
    assert_eq!(a.devices.has_pending_tick(), b.devices.has_pending_tick());
    assert_eq!(a.devices.overflow_seen(), b.devices.overflow_seen());
    assert_eq!(a.device_error, b.device_error);
    assert_eq!(a.presented_frames, b.presented_frames);
    assert_eq!(
        a.rgb565_display.as_ref().and_then(|d| d.snapshot()),
        b.rgb565_display.as_ref().and_then(|d| d.snapshot())
    );
}
fn differential(mut batch: Fixture, mut reference: Fixture, budget: u64) {
    for _ in 0..2000 {
        let before = batch.vm.executed_instructions;
        let result = batch.vm.run_batch(budget);
        let executed = batch.vm.executed_instructions - before;
        for _ in 0..executed {
            reference.vm.step().unwrap();
        }
        if let Err(error) = result {
            assert_eq!(reference.vm.step().unwrap_err(), error);
            same(&batch.vm, &reference.vm);
            return;
        }
        same(&batch.vm, &reference.vm);
        if !matches!(batch.vm.state, VMState::Running) {
            return;
        }
        assert_ne!(executed, 0, "unexpected unhandled host boundary");
    }
    panic!("fixture did not terminate");
}

#[test]
fn generated_arithmetic_ram_and_rng_match_at_every_batch_boundary() {
    let mut seed = 0x183bef25_u32;
    let mut next = || {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        seed
    };
    let mut code = Vec::new();
    for n in 0..500 {
        let op = [
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 26, 27, 28, 29, 30, 31,
        ][next() as usize % 22];
        let rd = 5 + next() as u16 % 23;
        let rs = next() as u16 % 32;
        let rt = next() as u16 % 32;
        code.push(i(op, rd, rs, rt));
        if n % 7 == 0 {
            code.push(i(18, rd, 28, 29));
            code.push(i(17, rd, 28, 29));
        }
        if n % 13 == 0 {
            code.push(i(17, rd, 0, 31));
        } // Observable RNG reads.
        if n % 17 == 0 {
            code.push(i(op, 48, rs, rt));
        } // Invalid register is a no-op.
    }
    code.push(i(0, 0, 0, 0));
    for budget in [1, 2, 7, 31, 1024] {
        let make = || {
            let mut f = Fixture::new(64, code.clone());
            f.vm.registers[28] = 3;
            f.vm.registers[29] = 2000;
            f.vm.registers[31] = 4;
            f.vm.set_rng_seed(12345);
            f
        };
        differential(make(), make(), budget);
    }
}

#[test]
fn branches_calls_pc_writes_and_bank_fallthrough_match() {
    let mut code = vec![i(66, 0, 0, 0); 12];
    code[0] = i(14, 7, 4, 0);
    code[1] = i(10, 7, 7, 65535);
    code[2] = i(22, 7, 0, 65535);
    code[3] = i(19, 3, 1, 2);
    code[4] = i(14, 1, 6, 0); // Write PC directly; normal increment still follows.
    code[8] = i(0, 0, 0, 0);
    code[10] = i(20, 0, 4, 3);
    differential(Fixture::new(8, code.clone()), Fixture::new(8, code), 1024);
    let mut code = vec![i(66, 0, 0, 0); 17];
    code[0] = i(14, 2, 1, 0); // Write PCB directly.
    code[9] = i(14, 1, 6, 0);
    code[16] = i(0, 0, 0, 0);
    differential(Fixture::new(8, code.clone()), Fixture::new(8, code), 1024);
}

#[test]
fn storc_replaces_the_next_instruction_and_can_grow_code_during_a_batch() {
    for grow in [false, true] {
        let make = || {
            let mut f = Fixture::new(
                16,
                vec![
                    i(33, 0, 28, 29),
                    if grow { i(19, 0, 2, 5) } else { i(66, 0, 0, 0) },
                    i(0, 0, 0, 0),
                ],
            );
            f.vm.registers[28] = if grow { 2 } else { 0 };
            f.vm.registers[29] = if grow { 5 } else { 1 };
            f.vm.registers[11] = if grow { 0 } else { 14 };
            f.vm.registers[12] = 7;
            f.vm.registers[13] = 1234;
            f
        };
        differential(make(), make(), 1024);
    }
}

#[test]
fn irq_store_dispatch_and_acknowledgement_keep_instruction_boundaries() {
    let make = || {
        let mut code = vec![i(66, 0, 0, 0); 69];
        code[0] = i(18, 7, 0, 8);
        code[1] = i(14, 11, 42, 0);
        code[2] = i(0, 0, 0, 0);
        code[66] = i(10, 9, 9, 1);
        code[67] = i(18, 7, 0, 10);
        code[68] = i(0x22, 0, 0, 0);
        let mut f = Fixture::new(64, code);
        f.vm.registers[7] = 1;
        f.vm.registers[8] = 21;
        f.vm.registers[10] = 23;
        for (offset, value) in [(22, 1), (24, 1), (25, 2)] {
            f.vm.write_cell(0, offset, value).unwrap();
        }
        f
    };
    differential(make(), make(), 1024);
}

#[test]
fn boundary_classification_honors_zero_and_invalid_registers() {
    let mut f = Fixture::new(64000, vec![i(18, 48, 0, 8), i(10, 7, 7, 1), i(0, 0, 0, 0)]);
    f.vm.registers[8] = 31;
    assert_eq!(f.vm.run_batch(1024).unwrap(), 3); // Invalid STORE does nothing.
    let mut f = Fixture::new(64000, vec![i(18, 7, 0, 8), i(10, 7, 7, 1)]);
    f.vm.registers[0] = 999;
    f.vm.registers[8] = 31;
    assert_eq!(f.vm.run_batch(1024).unwrap(), 1); // Actual bank is R0, not 999.
    assert_eq!(f.vm.registers[7], 0);
    let mut f = Fixture::new(64000, vec![i(18, 7, 0, 8), i(10, 7, 7, 1), i(0, 0, 0, 0)]);
    f.vm.registers[8] = 1000;
    assert_eq!(f.vm.run_batch(1024).unwrap(), 3); // Bank-zero ordinary RAM stays in batch.
}

#[test]
fn errors_and_debug_breakpoints_match_single_step() {
    for failing in [i(255, 0, 0, 0), i(17, 7, 28, 29), i(32, 0, 28, 29)] {
        let make = || {
            let mut f = Fixture::new(64000, vec![i(10, 7, 7, 1), failing]);
            f.vm.registers[28] = 65535;
            f.vm.registers[29] = 65535;
            f
        };
        differential(make(), make(), 1024);
    }
    differential(
        Fixture::new(64000, vec![i(66, 0, 0, 0)]),
        Fixture::new(64000, vec![i(66, 0, 0, 0)]),
        1024,
    );
    let make = || {
        let mut f = Fixture::new(64000, vec![i(25, 0, 0, 0), i(14, 7, 9, 0)]);
        f.vm.debug_mode = true;
        f
    };
    differential(make(), make(), 1024);
}

#[test]
fn runner_instruction_limits_halt_and_error_precedence_match() {
    for limit in [0, 1, 1023, 1024, 1025, 2050] {
        for last in [i(0, 0, 0, 0), i(255, 0, 0, 0), i(18, 7, 0, 8)] {
            let make = || {
                let mut f = Fixture::new(64000, vec![i(10, 9, 9, 1); 2049]);
                f.vm.instructions.push(last);
                f.vm.registers[7] = 99;
                f.vm.registers[8] = 6; // Unsupported display mode records a device error.
                f
            };
            let (mut batch, mut reference) = (make(), make());
            let options = RunOptions {
                max_instructions: Some(limit),
                ..Default::default()
            };
            assert_eq!(
                runner::run(&mut batch.vm, &options),
                runner::run_reference(&mut reference.vm, &options)
            );
            same(&batch.vm, &reference.vm);
        }
    }
}

#[test]
fn capture_stops_at_first_present_before_following_store_and_limit() {
    let make = || {
        let mut f = Fixture::new(
            64000,
            vec![
                i(18, 7, 0, 8),
                i(18, 9, 10, 11),
                i(18, 7, 0, 8),
                i(0, 0, 0, 0),
            ],
        );
        f.vm.write_cell(0, 6, 4).unwrap();
        f.vm.memory[100..108].copy_from_slice(&[1, 8, 1, 1, 1, 1, 0, 0]);
        f.vm.memory[64000] = 0xf800;
        f.vm.write_cell(0, 27, 0).unwrap();
        f.vm.write_cell(0, 28, 100).unwrap();
        f.vm.registers[7] = 1;
        f.vm.registers[8] = 29;
        f.vm.registers[9] = 0x07e0;
        f.vm.registers[10] = 1;
        f
    };
    for frame in [1, 2] {
        let (mut batch, mut reference) = (make(), make());
        let a = batch.path.with_extension("rgb565");
        let b = reference.path.with_extension("rgb565");
        let options = |path| RunOptions {
            raw_frame: Some(path),
            capture_frame: frame,
            max_instructions: Some(if frame == 1 { 1 } else { 3 }),
            ..Default::default()
        };
        assert_eq!(
            runner::run(&mut batch.vm, &options(a.clone())).unwrap(),
            "captured_frame"
        );
        assert_eq!(
            runner::run_reference(&mut reference.vm, &options(b.clone())).unwrap(),
            "captured_frame"
        );
        same(&batch.vm, &reference.vm);
        assert_eq!(fs::read(&a).unwrap(), fs::read(&b).unwrap());
        fs::remove_file(a).unwrap();
        fs::remove_file(b).unwrap();
    }
}

#[test]
fn ticks_limits_and_overflow_leave_the_same_pending_state() {
    for policy in 0..3 {
        let make = || {
            let mut f = Fixture::new(64000, vec![i(18, 7, 0, 8), i(10, 9, 9, 1), i(0, 0, 0, 0)]);
            f.vm.memory[100..108].copy_from_slice(&[1, 8, 0, 0, 0, 0, 0, 0]);
            f.vm.write_cell(0, 27, 0).unwrap();
            f.vm.write_cell(0, 28, 100).unwrap();
            f.vm.registers[7] = 3;
            f.vm.registers[8] = 29;
            if policy == 2 {
                let events = (0..300)
                    .map(|n| format!(r#"{{"tick":1,"key":4,"down":{}}}"#, n % 2 == 0))
                    .collect::<Vec<_>>()
                    .join(",");
                f.vm.devices
                    .set_script(InputScript::from_json(&format!("[{events}]")).unwrap())
                    .unwrap();
            }
            f
        };
        let (mut batch, mut reference) = (make(), make());
        let options = RunOptions {
            max_instructions: if policy == 0 { Some(1) } else { None },
            max_ticks: if policy == 1 { Some(0) } else { None },
            deterministic: true,
            ..Default::default()
        };
        assert_eq!(
            runner::run(&mut batch.vm, &options),
            runner::run_reference(&mut reference.vm, &options)
        );
        same(&batch.vm, &reference.vm);
        assert_eq!(batch.vm.executed_instructions, 1);
        assert_eq!(batch.vm.registers[9], 0);
    }
}

#[test]
fn resumed_runner_polls_at_original_1024_boundary_and_frequency_path_matches() {
    for frequency in [None, Some(10_000_000)] {
        let make = || {
            let mut f = Fixture::new(64000, vec![i(10, 7, 7, 1), i(19, 0, 0, 0)]);
            f.vm.executed_instructions = 13;
            f
        };
        let options = || {
            let (tx, rx) = mpsc::channel();
            tx.send(Ok(ControlCommand::Quit)).unwrap();
            RunOptions {
                control: Some(rx),
                frequency,
                ..Default::default()
            }
        };
        let (mut batch, mut reference) = (make(), make());
        assert_eq!(runner::run(&mut batch.vm, &options()).unwrap(), "host_stop");
        assert_eq!(
            runner::run_reference(&mut reference.vm, &options()).unwrap(),
            "host_stop"
        );
        same(&batch.vm, &reference.vm);
        assert_eq!(batch.vm.executed_instructions, 1024);
    }
    let (mut batch, mut reference) = (
        Fixture::new(64000, vec![i(19, 0, 0, 0)]),
        Fixture::new(64000, vec![i(19, 0, 0, 0)]),
    );
    let options = RunOptions {
        timeout: Some(Duration::ZERO),
        ..Default::default()
    };
    assert_eq!(
        runner::run(&mut batch.vm, &options),
        runner::run_reference(&mut reference.vm, &options)
    );
    same(&batch.vm, &reference.vm);
}
