use rvm::{
    devices::InputScript,
    runner::{self, ClockPolicy, ControlCommand, RunOptions},
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
    time::{Duration, Instant},
};
static ID: AtomicU64 = AtomicU64::new(0);
struct Fixture {
    vm: VM,
    path: PathBuf,
}
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "rvm-policy-{}-{}.img",
            std::process::id(),
            ID.fetch_add(1, Ordering::Relaxed)
        ));
        let mut vm = VM::with_options(64000, 128000, Some(path.clone()));
        vm.host_input_enabled = false;
        vm.state = VMState::Running;
        vm.instructions = vec![Instr {
            opcode: 0,
            word0: 0,
            word1: 0,
            word2: 0,
            word3: 0,
        }];
        Self { vm, path }
    }
    fn request_tick(&mut self) {
        self.vm.memory[100..108].copy_from_slice(&[1, 8, 0, 0, 0, 0, 0, 0]);
        self.vm.write_cell(0, 27, 0).unwrap();
        self.vm.write_cell(0, 28, 100).unwrap();
        self.vm.write_cell(0, 29, 3).unwrap();
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
    }
}

#[test]
fn stepped_grant_latches_input_and_runs_until_next_boundary() {
    let mut f = Fixture::new();
    f.request_tick();
    let (tx, rx) = mpsc::channel();
    tx.send(Ok(ControlCommand::Key {
        key: "W".into(),
        down: true,
    }))
    .unwrap();
    tx.send(Ok(ControlCommand::Step { ticks: 1 })).unwrap();
    let options = RunOptions {
        clock: ClockPolicy::Stepped,
        control: Some(rx),
        ..Default::default()
    };
    assert_eq!(runner::run(&mut f.vm, &options).unwrap(), "halted");
    assert_eq!(f.vm.devices.tick(), 1);
    assert!(f.vm.devices.held(26));
    assert_eq!(f.vm.executed_instructions, 1);
}

#[test]
fn stepped_wait_has_timeout_and_eof_diagnostics() {
    let mut f = Fixture::new();
    f.request_tick();
    let (_tx, rx) = mpsc::channel();
    let options = RunOptions {
        clock: ClockPolicy::Stepped,
        control: Some(rx),
        timeout: Some(Duration::from_millis(5)),
        ..Default::default()
    };
    assert!(runner::run(&mut f.vm, &options)
        .unwrap_err()
        .contains("Wall-time"));
    assert_eq!(f.vm.devices.tick(), 0);
    let (tx, rx) = mpsc::channel();
    drop(tx);
    let options = RunOptions {
        clock: ClockPolicy::Stepped,
        control: Some(rx),
        ..Default::default()
    };
    assert!(runner::run(&mut f.vm, &options)
        .unwrap_err()
        .contains("controller closed"));
}

#[test]
fn paced_tick_waits_and_capture_does_not_advance_time() {
    let mut f = Fixture::new();
    f.request_tick();
    let before = Instant::now();
    let options = RunOptions {
        clock: ClockPolicy::Paced,
        ..Default::default()
    };
    runner::run(&mut f.vm, &options).unwrap();
    assert!(before.elapsed() >= Duration::from_millis(28));
    assert_eq!(f.vm.devices.millis(), 28);
    assert_eq!(f.vm.devices.tick(), 1);
}

#[test]
fn budgets_and_missing_frames_fail_and_tick_limit_preserves_pending_request() {
    let mut f = Fixture::new();
    let options = RunOptions {
        max_instructions: Some(0),
        ..Default::default()
    };
    assert!(runner::run(&mut f.vm, &options)
        .unwrap_err()
        .contains("Instruction limit"));
    let options = RunOptions {
        screenshot: Some(f.path.with_extension("png")),
        capture_frame: 1,
        ..Default::default()
    };
    assert!(runner::run(&mut f.vm, &options)
        .unwrap_err()
        .contains("before frame 1"));
    f.vm.state = VMState::Running;
    f.request_tick();
    let options = RunOptions {
        max_ticks: Some(0),
        ..Default::default()
    };
    assert_eq!(runner::run(&mut f.vm, &options).unwrap(), "tick_limit");
    assert!(f.vm.devices.has_pending_tick());
}

#[test]
fn deterministic_input_overflow_fails_even_if_state_was_resynchronized() {
    let mut f = Fixture::new();
    let mut events = Vec::new();
    for i in 0..300 {
        events.push(format!(r#"{{"tick":1,"key":"A","down":{}}}"#, i % 2 == 0));
    }
    f.vm.devices
        .set_script(InputScript::from_json(&format!("[{}]", events.join(","))).unwrap())
        .unwrap();
    f.request_tick();
    let options = RunOptions {
        deterministic: true,
        ..Default::default()
    };
    assert!(runner::run(&mut f.vm, &options)
        .unwrap_err()
        .contains("Input queue overflow"));
}

#[test]
fn malformed_control_commands_are_rejected() {
    assert!(serde_json::from_str::<ControlCommand>(r#"{"op":"step","ticks":1,"typo":1}"#).is_err());
    assert!(serde_json::from_str::<ControlCommand>(r#"{"op":"key","key":"W"}"#).is_err());
}

#[test]
fn piped_key_step_release_commands_preserve_hold_duration() {
    let mut f = Fixture::new();
    f.request_tick();
    f.vm.registers[8] = 3;
    f.vm.registers[9] = 29;
    f.vm.instructions = vec![
        Instr {
            opcode: 0x12,
            word0: 0,
            word1: 8,
            word2: 0,
            word3: 9,
        },
        Instr {
            opcode: 0x13,
            word0: 0,
            word1: 0,
            word2: 0,
            word3: 0,
        },
    ];
    let (tx, rx) = mpsc::channel();
    for command in [
        ControlCommand::Key {
            key: "W".into(),
            down: true,
        },
        ControlCommand::Step { ticks: 2 },
        ControlCommand::Key {
            key: "W".into(),
            down: false,
        },
        ControlCommand::Step { ticks: 1 },
    ] {
        tx.send(Ok(command)).unwrap();
    }
    let options = RunOptions {
        clock: ClockPolicy::Stepped,
        control: Some(rx),
        max_ticks: Some(3),
        ..Default::default()
    };
    assert_eq!(runner::run(&mut f.vm, &options).unwrap(), "tick_limit");
    // Inspect queued guest events without granting the pending fourth tick.
    // A cloned device still has the pending request; use its complete_tick then POP.
    f.vm.complete_tick(Vec::new()).unwrap();
    f.vm.write_cell(0, 29, 5).unwrap();
    assert_eq!(&f.vm.memory[102..108], &[1, 26, 1, 1, 0, 0]);
    f.vm.write_cell(0, 29, 5).unwrap();
    assert_eq!(&f.vm.memory[102..108], &[1, 26, 0, 3, 0, 0]);
}

#[test]
fn capture_cannot_overwrite_its_baseline() {
    let mut f = Fixture::new();
    fs::write(&f.path, [0u8, 0]).unwrap();
    let options = RunOptions {
        raw_frame: Some(f.path.clone()),
        expect_frame: Some(f.path.clone()),
        capture_frame: 1,
        ..Default::default()
    };
    assert!(runner::run(&mut f.vm, &options)
        .unwrap_err()
        .contains("overwrite"));
    assert_eq!(fs::read(&f.path).unwrap(), vec![0, 0]);
}
