//! Host-policy regressions use tiny BF transport programs, without running a
//! Ripple interpreter or a Doom workload. Every potentially looping case has
//! independent dispatch and wall-time limits.
use bf_wasm::engine::{Profile, Program};
use rbf::{
    devices::BfDevices,
    image::{Layout, StridedMemory, LANES, ORIGIN, RAM_LANE},
    runner::{Host, RunOptions},
};
use rvm::{
    constants::*,
    devices::{GuestMemory, COMMAND_INPUT_POP, REGISTER_COMMAND},
    runner::{ClockPolicy, ControlCommand},
};
use std::{
    io::{self, Write},
    path::PathBuf,
    sync::{
        atomic::{AtomicU64, Ordering},
        mpsc, Arc,
    },
    thread,
    time::Duration,
};

const HEADER: &str = "BFHOST 1 BANK_SIZE 64 MEMORY_BANKS 4 INSTRUCTIONS 1 ENTRY 0 ORIGIN 8 TAPE_CELLS 2312 END_BFHOST\n$?!";

fn host() -> Host {
    let (layout, _) = Layout::parse(HEADER, 2312).unwrap();
    let mut devices = BfDevices::new(layout.bank_size, 35, 1, None).unwrap();
    devices.host_input_enabled = false;
    Host::new(layout, devices).unwrap()
}

fn limits() -> RunOptions {
    RunOptions {
        max_dispatches: Some(100_000),
        timeout: Some(Duration::from_secs(2)),
        deterministic: true,
        ..RunOptions::default()
    }
}

fn run(
    host: &mut Host,
    source: &str,
    options: &RunOptions,
    output: &mut impl Write,
) -> Result<String, String> {
    let program = Program::parse(source, true, true).unwrap();
    host.run(&program, options, output)
}

#[derive(Default)]
struct TapeProgram {
    source: String,
    pointer: usize,
}
impl TapeProgram {
    fn at(&mut self, pointer: usize) {
        let direction = if pointer >= self.pointer { '>' } else { '<' };
        self.source
            .extend(std::iter::repeat(direction).take(pointer.abs_diff(self.pointer)));
        self.pointer = pointer;
    }
    fn ram(&mut self, address: usize) {
        self.at(ORIGIN + address * LANES + RAM_LANE);
    }
    fn value(&mut self, value: u16) {
        self.source.push_str("[-]");
        if value < 32768 {
            self.source
                .extend(std::iter::repeat('+').take(usize::from(value)));
        } else {
            self.source
                .extend(std::iter::repeat('-').take(65536 - usize::from(value)));
        }
    }
    fn store(&mut self, address: usize, value: u16) {
        self.ram(address);
        self.value(value);
        self.source.push('!');
    }
}

static NEXT_DIRECTORY: AtomicU64 = AtomicU64::new(0);
struct Directory(PathBuf);
impl Directory {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "rbf-runner-{}-{}",
            std::process::id(),
            NEXT_DIRECTORY.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&path).unwrap();
        Self(path)
    }
}
impl Drop for Directory {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

#[test]
fn dispatch_limit_stops_a_loop_before_any_cpu_hook() {
    let mut host = host();
    let options = RunOptions {
        max_dispatches: Some(17),
        ..limits()
    };
    let error = run(&mut host, "+[]", &options, &mut Vec::new()).unwrap_err();
    assert!(error.contains("dispatch limit"), "{error}");
    assert_eq!(host.machine.dispatches(), 17);
    assert_eq!(host.instructions, 0);
    assert_eq!(host.machine.tape()[0], 1);
}

#[test]
fn profiling_preserves_output_tape_and_limits() {
    let mut plain = host();
    let mut profiled = host();
    profiled.profile = Some(Profile::default());
    let mut program = TapeProgram::default();
    program.store(HDR_TTY_OUT, 65);
    program.at(0);
    program.source.push_str("+[]");
    let options = RunOptions { max_dispatches: Some(100), ..limits() };
    let mut plain_output = Vec::new();
    let mut profiled_output = Vec::new();
    let plain_result = run(&mut plain, &program.source, &options, &mut plain_output);
    let profiled_result = run(&mut profiled, &program.source, &options, &mut profiled_output);
    assert_eq!(plain_result, profiled_result);
    assert!(plain_result.unwrap_err().contains("dispatch limit"));
    assert_eq!(plain_output, b"A");
    assert_eq!(profiled_output, plain_output);
    assert_eq!(plain.machine.tape(), profiled.machine.tape());
    assert_eq!(plain.machine.pointer(), profiled.machine.pointer());
    let report = profiled.profile.unwrap().report();
    assert_eq!(report.dispatches, 100);
    assert!(report.operations.iter().any(|operation| operation.attempts > 0));
}

#[test]
fn timeout_and_cancellation_work_before_cpu_initialization_or_hooks() {
    let mut timed = host();
    let options = RunOptions {
        timeout: Some(Duration::ZERO),
        ..limits()
    };
    let error = run(&mut timed, "+[]", &options, &mut Vec::new()).unwrap_err();
    assert!(error.contains("Wall-time limit"), "{error}");
    assert_eq!(timed.machine.dispatches(), 0);

    let mut cancelled = host();
    let options = RunOptions {
        max_dispatches: None,
        ..limits()
    };
    let flag = Arc::clone(&options.cancelled);
    let worker = thread::spawn(move || {
        thread::sleep(Duration::from_millis(10));
        flag.store(true, Ordering::Relaxed);
    });
    let result = run(&mut cancelled, "+[]", &options, &mut Vec::new());
    worker.join().unwrap();
    assert_eq!(result.unwrap(), "host_stop");
    assert_eq!(cancelled.instructions, 0);
}

#[test]
fn timeout_does_not_hide_a_failed_final_output_flush() {
    struct FailedFlush;
    impl Write for FailedFlush {
        fn write(&mut self, bytes: &[u8]) -> io::Result<usize> { Ok(bytes.len()) }
        fn flush(&mut self) -> io::Result<()> { Err(io::Error::other("lost output")) }
    }
    let options = RunOptions { timeout: Some(Duration::ZERO), ..limits() };
    let error = run(&mut host(), "+[]", &options, &mut FailedFlush).unwrap_err();
    assert!(error.contains("Wall-time limit exceeded"));
    assert!(error.contains("Guest output: lost output"));
}

#[test]
fn instruction_limit_stops_before_instruction_body() {
    let mut host = host();
    let options = RunOptions {
        max_instructions: Some(0),
        ..limits()
    };
    let error = run(&mut host, "$+", &options, &mut Vec::new()).unwrap_err();
    assert!(error.contains("instruction limit"), "{error}");
    assert_eq!(host.instructions, 0);
    assert_eq!(host.machine.tape()[0], 0);
}

#[test]
fn instruction_hooks_validate_code_extent_and_physical_layout() {
    for (bank, pc, pointer, message) in [
        (1, 0, ORIGIN + 8 * 40, "outside its code image"),
        (0, 2, ORIGIN + 8 * 48, "outside its code image"),
        (0, 0, 0, "declared BF CPU layout"),
    ] {
        let mut host = host();
        host.machine.tape_mut()[ORIGIN + 25] = bank;
        host.machine.tape_mut()[ORIGIN + 17] = pc;
        let source = format!("{}$", ">".repeat(pointer));
        let error = run(&mut host, &source, &limits(), &mut Vec::new()).unwrap_err();
        assert!(error.contains(message), "{error}");
        assert_eq!(host.instructions, 0);
    }
    let mut host = host();
    host.machine.tape_mut()[ORIGIN + 17] = 1; // Implicit HALT after the image.
    let source = format!("{}$", ">".repeat(ORIGIN + 8 * 44));
    assert_eq!(
        run(&mut host, &source, &limits(), &mut Vec::new()).unwrap(),
        "halted"
    );
    assert_eq!(host.instructions, 1);
}

#[test]
fn instruction_hooks_accept_banks_and_raw_return_offsets() {
    let header = "BFHOST 1 BANK_SIZE 64 MEMORY_BANKS 4 INSTRUCTIONS 129 ENTRY 128 ORIGIN 8 TAPE_CELLS 5000 END_BFHOST $?!";
    for (bank, pc, index) in [(1, 0, 64), (0, 64, 64), (2, 0, 128), (2, 1, 129)] {
        let (layout, _) = Layout::parse(header, 5000).unwrap();
        let devices = BfDevices::new(64, 35, 1, None).unwrap();
        let mut host = Host::new(layout, devices).unwrap();
        host.machine.tape_mut()[ORIGIN + 25] = bank;
        host.machine.tape_mut()[ORIGIN + 17] = pc;
        let source = format!("{}$", ">".repeat(ORIGIN + 8 * (40 + 4 * index)));
        assert_eq!(
            run(&mut host, &source, &limits(), &mut Vec::new()).unwrap(),
            "halted"
        );
        assert_eq!(host.instructions, 1);
    }
    // A physical pointer in bank zero cannot masquerade as an instruction in bank one.
    let (layout, _) = Layout::parse(header, 5000).unwrap();
    let mut host = Host::new(layout, BfDevices::new(64, 35, 1, None).unwrap()).unwrap();
    host.machine.tape_mut()[ORIGIN + 25] = 1;
    let source = format!("{}$", ">".repeat(ORIGIN + 8 * 40));
    let error = run(&mut host, &source, &limits(), &mut Vec::new()).unwrap_err();
    assert!(error.contains("declared BF CPU layout"), "{error}");
}

#[test]
fn malformed_hooks_and_raw_bf_io_fail_explicitly() {
    for pointer in [0, ORIGIN + RAM_LANE - 1, ORIGIN + 256 * LANES + RAM_LANE] {
        for marker in ['?', '!'] {
            let mut host = host();
            let mut program = TapeProgram::default();
            program.at(pointer);
            program.source.push(marker);
            let error = run(&mut host, &program.source, &limits(), &mut Vec::new()).unwrap_err();
            assert!(error.contains("Device hook"), "{error}");
            assert_eq!(host.devices.devices.tick(), 0);
            assert_eq!(host.devices.presented_frames, 0);
        }
    }
    for (source, message) in [(".", "Unexpected BF output"), (",", "Unexpected BF input")] {
        let error = run(&mut host(), source, &limits(), &mut Vec::new()).unwrap_err();
        assert!(error.contains(message), "{error}");
    }
}

#[test]
fn repeated_write_hooks_preserve_raw_octets_and_prefix_on_error() {
    let mut host = host();
    let mut program = TapeProgram::default();
    program.store(HDR_TTY_OUT, 0x0180);
    program.source.push('!'); // Identical value is still another STORE.
    program.store(HDR_TTY_OUT, 255);
    program.store(HDR_TTY_OUT, 0);
    program.at(0);
    program.source.push('?'); // A later invalid hook must not discard output.
    let mut output = Vec::new();
    assert!(run(&mut host, &program.source, &limits(), &mut output).is_err());
    assert_eq!(output, [128, 128, 255, 0]);
}

#[test]
fn tty_read_hook_supplies_bytes_before_following_bf_work() {
    let mut host = host();
    host.devices.push_input(&[0x80, 0xff]);
    let mut program = TapeProgram::default();
    let output_pointer = ORIGIN + RAM_LANE;
    for _ in 0..2 {
        program.at(output_pointer);
        program.value(0);
        program.ram(HDR_TTY_IN_POP);
        program.source.push('?');
        let input_pointer = program.pointer;
        program.source.push('[');
        program.source.push('-');
        program.at(output_pointer);
        program.source.push('+');
        program.at(input_pointer);
        program.source.push(']');
        program.at(output_pointer);
        program.source.push('!');
    }
    let mut output = Vec::new();
    assert_eq!(
        run(&mut host, &program.source, &limits(), &mut output).unwrap(),
        "halted"
    );
    assert_eq!(output, [128, 255]);
}

fn next_ticks(host: &mut Host, count: usize) -> String {
    {
        let mut memory = StridedMemory::new(host.machine.tape_mut(), 256);
        memory.write(63, 1);
        memory.write(64, 8);
    }
    let mut program = TapeProgram::default();
    program.store(28, 63);
    program.store(29, 3);
    program
        .source
        .extend(std::iter::repeat('!').take(count - 1));
    program.source
}

#[test]
fn stepped_controller_closure_fails_when_a_tick_is_still_needed() {
    let mut host = host();
    let source = next_ticks(&mut host, 1);
    let (tx, rx) = mpsc::channel();
    drop(tx);
    let options = RunOptions {
        clock: ClockPolicy::Stepped,
        control: Some(rx),
        ..limits()
    };
    let error = run(&mut host, &source, &options, &mut Vec::new()).unwrap_err();
    assert!(error.contains("controller closed"), "{error}");
    assert_eq!(host.devices.devices.tick(), 0);
    assert!(host.devices.devices.has_pending_tick());
}

#[test]
fn stepped_key_commands_apply_in_order_at_separate_tick_grants() {
    let mut host = host();
    let source = next_ticks(&mut host, 2);
    let (tx, rx) = mpsc::channel();
    for command in [
        ControlCommand::Key {
            key: "A".into(),
            down: true,
        },
        ControlCommand::Step { ticks: 1 },
        ControlCommand::Key {
            key: "A".into(),
            down: false,
        },
        ControlCommand::Step { ticks: 1 },
    ] {
        tx.send(Ok(command)).unwrap();
    }
    drop(tx);
    let options = RunOptions {
        clock: ClockPolicy::Stepped,
        control: Some(rx),
        ..limits()
    };
    assert_eq!(
        run(&mut host, &source, &options, &mut Vec::new()).unwrap(),
        "halted"
    );
    assert_eq!(host.devices.devices.tick(), 2);
    assert!(!host.devices.devices.held(4));
    let mut memory = StridedMemory::new(host.machine.tape_mut(), 256);
    for (down, tick) in [(1, 1), (0, 2)] {
        host.devices
            .write_mmio(REGISTER_COMMAND, COMMAND_INPUT_POP, &mut memory)
            .unwrap();
        assert_eq!(
            (65..70).map(|i| memory.read(i)).collect::<Vec<_>>(),
            [1, 4, down, tick, 0]
        );
    }
}

fn present_one_pixel() -> TapeProgram {
    let mut program = TapeProgram::default();
    program.store(HDR_DISP_RESOLUTION, 0x0101);
    program.store(HDR_DISP_MODE, DISP_RGB565);
    program.store(33, 0xf800);
    program.store(HDR_DISP_FLUSH, 1);
    program
}

fn text_host() -> Host {
    let header = "BFHOST 1 BANK_SIZE 2048 MEMORY_BANKS 1 INSTRUCTIONS 1 ENTRY 0 ORIGIN 8 TAPE_CELLS 16648 END_BFHOST $?!";
    let (layout, _) = Layout::parse(header, 16648).unwrap();
    let mut devices = BfDevices::new(2048, 35, 1, None).unwrap();
    devices.host_input_enabled = false;
    Host::new(layout, devices).unwrap()
}

fn present_text() -> TapeProgram {
    let mut program = TapeProgram::default();
    program.store(HDR_DISP_MODE, DISP_TEXT40);
    program.store(HDR_DISP_CTL, DISP_ENABLE | DISP_CLEAR);
    program.store(TEXT40_BASE_WORD, 0x1741);
    program.store(TEXT40_LAST_WORD, 0xf842);
    program.store(HDR_DISP_FLUSH, 1);
    program
}

#[test]
fn text_capture_preserves_cells_and_stops_at_requested_flush() {
    let directory = Directory::new();
    let raw = directory.0.join("frame.text40");
    let options = RunOptions {
        text_frame: Some(raw.clone()),
        ..limits()
    };
    let mut source = present_text();
    source.at(0);
    source.source.push('?'); // Capture must stop before this later invalid hook.
    assert_eq!(
        run(&mut text_host(), &source.source, &options, &mut Vec::new()).unwrap(),
        "captured_frame"
    );
    let bytes = std::fs::read(raw).unwrap();
    assert_eq!(bytes.len(), TEXT40_WORDS * 2);
    assert_eq!(&bytes[..2], &[0x41, 0x17]);
    assert_eq!(&bytes[bytes.len() - 2..], &[0x42, 0xf8]);
    assert!(bytes[2..bytes.len() - 2].iter().all(|&byte| byte == 0));
}

#[test]
fn text_capture_rejects_wrong_frame_type_missing_frame_and_mixed_formats() {
    let directory = Directory::new();
    let raw = directory.0.join("frame.text40");
    let mut options = RunOptions {
        text_frame: Some(raw.clone()),
        ..limits()
    };
    let error = run(
        &mut host(),
        &present_one_pixel().source,
        &options,
        &mut Vec::new(),
    )
    .unwrap_err();
    assert!(error.contains("not a presented TEXT40 frame"), "{error}");
    options.capture_frame = 2;
    let error = run(
        &mut text_host(),
        &present_text().source,
        &options,
        &mut Vec::new(),
    )
    .unwrap_err();
    assert!(error.contains("halted before frame 2"), "{error}");
    options.raw_frame = Some(directory.0.join("rgb.rgb565"));
    let error = run(
        &mut text_host(),
        &present_text().source,
        &options,
        &mut Vec::new(),
    )
    .unwrap_err();
    assert!(error.contains("cannot be combined"), "{error}");
    assert!(!raw.exists());
    options.text_frame = None;
    options.capture_frame = 1;
    let error = run(
        &mut text_host(),
        &present_text().source,
        &options,
        &mut Vec::new(),
    )
    .unwrap_err();
    assert!(error.contains("No presented RGB565 frame"), "{error}");
}

#[test]
fn requested_frame_stops_before_later_guest_fault_and_short_capture_fails() {
    let directory = Directory::new();
    let raw = directory.0.join("frame.rgb565");
    let options = RunOptions {
        raw_frame: Some(raw.clone()),
        ..limits()
    };
    let mut host = host();
    let mut source = present_one_pixel();
    source.at(0);
    source.source.push('?');
    assert_eq!(
        run(&mut host, &source.source, &options, &mut Vec::new()).unwrap(),
        "captured_frame"
    );
    assert_eq!(std::fs::read(raw).unwrap(), [0, 248]);

    let missing = directory.0.join("missing.rgb565");
    let options = RunOptions {
        raw_frame: Some(missing.clone()),
        capture_frame: 2,
        ..limits()
    };
    let error = run(
        &mut self::host(),
        &present_one_pixel().source,
        &options,
        &mut Vec::new(),
    )
    .unwrap_err();
    assert!(error.contains("halted before frame 2"), "{error}");
    assert!(!missing.exists());
}

#[test]
fn capture_baseline_and_output_errors_are_not_silent_successes() {
    let directory = Directory::new();
    let baseline = directory.0.join("baseline.rgb565");
    std::fs::write(&baseline, [0, 248]).unwrap();
    let options = RunOptions {
        raw_frame: Some(baseline.clone()),
        expect_frame: Some(baseline.clone()),
        ..limits()
    };
    let error = run(
        &mut host(),
        &present_one_pixel().source,
        &options,
        &mut Vec::new(),
    )
    .unwrap_err();
    assert!(error.contains("overwrite"), "{error}");
    assert_eq!(std::fs::read(baseline).unwrap(), [0, 248]);

    struct BrokenWriter;
    impl Write for BrokenWriter {
        fn write(&mut self, _: &[u8]) -> io::Result<usize> {
            Err(io::Error::other("output unavailable"))
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }
    let mut program = TapeProgram::default();
    program.store(HDR_TTY_OUT, 65);
    let error = run(&mut host(), &program.source, &limits(), &mut BrokenWriter).unwrap_err();
    assert!(error.contains("output unavailable"), "{error}");
}
