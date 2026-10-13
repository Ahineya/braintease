//! Progress must remain observational: these tiny transport programs compare
//! guest output/state and exercise clock waits without wall-clock timing asserts.
use bf_wasm::engine::Program;
use rbf::{
    devices::BfDevices,
    image::{Layout, StridedMemory, LANES, ORIGIN, RAM_LANE},
    runner::{Host, RunOptions},
    telemetry::{Progress, Snapshot, Telemetry},
};
use rvm::{
    constants::*,
    devices::GuestMemory,
    runner::{ClockPolicy, ControlCommand},
    vm::Storage,
};
use std::{io, sync::mpsc, time::Duration};

fn host() -> Host {
    host_with_storage(None)
}

fn host_with_storage(storage: Option<Storage>) -> Host {
    let header = "BFHOST 1 BANK_SIZE 64 MEMORY_BANKS 4 INSTRUCTIONS 1 ENTRY 0 ORIGIN 8 TAPE_CELLS 2312 END_BFHOST $?!";
    let (layout, _) = Layout::parse(header, 2312).unwrap();
    let mut devices = BfDevices::new(64, 35, 1, storage).unwrap();
    devices.host_input_enabled = false;
    Host::new(layout, devices).unwrap()
}

fn options() -> RunOptions {
    RunOptions {
        max_dispatches: Some(20_000),
        timeout: Some(Duration::from_secs(2)),
        deterministic: true,
        ..RunOptions::default()
    }
}

#[derive(Default)]
struct Capture {
    updates: Vec<(Snapshot, Option<String>)>,
    step: Option<mpsc::Sender<Result<ControlCommand, String>>>,
}
impl Progress for Capture {
    fn update(&mut self, snapshot: Snapshot, status: Option<&str>) -> Result<(), String> {
        if snapshot.waiting_for_tick {
            if let Some(sender) = self.step.take() {
                sender.send(Ok(ControlCommand::Step { ticks: 1 })).unwrap();
            }
        }
        self.updates.push((snapshot, status.map(str::to_owned)));
        Ok(())
    }
}

#[derive(Default)]
struct Source {
    text: String,
    pointer: usize,
}
impl Source {
    fn at(&mut self, target: usize) {
        self.text.push_str(&if target >= self.pointer {
            ">".repeat(target - self.pointer)
        } else {
            "<".repeat(self.pointer - target)
        });
        self.pointer = target;
    }
    fn value(&mut self, value: u16) {
        self.text.push_str("[-]");
        self.text.push_str(&if value < 32768 {
            "+".repeat(value.into())
        } else {
            "-".repeat(65536 - usize::from(value))
        });
    }
    fn store(&mut self, address: usize, value: u16) {
        self.at(ORIGIN + address * LANES + RAM_LANE);
        self.value(value);
        self.text.push('!');
    }
    fn read(&mut self, address: usize) {
        self.at(ORIGIN + address * LANES + RAM_LANE);
        self.text.push('?');
    }
}

#[test]
fn disk_progress_observes_successful_transfers_without_changing_guest_state() {
    use std::sync::atomic::{AtomicU64, Ordering};
    static NEXT_DISK: AtomicU64 = AtomicU64::new(0);
    struct Disk(std::path::PathBuf);
    impl Disk {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "rbf-telemetry-disk-{}-{}.img", std::process::id(),
                NEXT_DISK.fetch_add(1, Ordering::Relaxed)
            ));
            std::fs::write(&path, [0x34, 0x12, 0x56]).unwrap();
            Self(path)
        }
        fn storage(&self) -> Storage { Storage::with_path(self.0.clone()).unwrap() }
    }
    impl Drop for Disk {
        fn drop(&mut self) { let _ = std::fs::remove_file(&self.0); }
    }
    let actual_disk = Disk::new();
    let plain_disk = Disk::new();
    let mut actual = host_with_storage(Some(actual_disk.storage()));
    let mut plain = host_with_storage(Some(plain_disk.storage()));
    let mut source = Source::default();
    source.read(HDR_STORE_DATA);
    source.store(HDR_STORE_BLOCK, 1);
    source.store(HDR_STORE_ADDR, 32767);
    source.read(HDR_STORE_DATA);
    source.read(HDR_STORE_DATA); // Out-of-range word still completes a device read.
    source.store(HDR_STORE_ADDR, 65535);
    source.read(HDR_STORE_DATA); // Auto-increment wraps to word zero in block one.
    source.store(HDR_STORE_DATA, 0xabcd);
    source.read(HDR_STORE_DATA);
    source.store(64 + HDR_STORE_DATA, 11); // Ordinary RAM in another bank.
    source.read(64 + HDR_STORE_DATA);
    source.store(HDR_TTY_OUT, 89);
    let program = Program::parse(&source.text, true, true).unwrap();
    let mut progress = Capture::default();
    let mut output = Vec::new();
    let mut plain_output = Vec::new();
    assert_eq!(
        actual.run_with_telemetry(&program, &options(), &mut output, &mut progress),
        plain.run(&program, &options(), &mut plain_output)
    );
    assert_eq!(output, b"Y");
    assert_eq!(output, plain_output);
    assert_eq!(actual.machine.tape(), plain.machine.tape());
    assert_eq!(actual.machine.dispatches(), plain.machine.dispatches());
    assert_eq!(std::fs::read(&actual_disk.0).unwrap(), std::fs::read(&plain_disk.0).unwrap());
    let state = progress.updates.last().unwrap().0;
    assert_eq!(state.disk_read_words, 5);
    assert_eq!(state.disk_write_words, 1);
    assert_eq!(state.last_disk_read, Some((1, 1)));
    assert_eq!(state.disk_position, Some((1, 2)));

    let mut absent = host();
    let mut progress = Capture::default();
    absent.run_with_telemetry(&program, &options(), &mut Vec::new(), &mut progress).unwrap();
    let state = progress.updates.last().unwrap().0;
    assert_eq!(state.disk_read_words, 0);
    assert_eq!(state.disk_write_words, 0);
    assert_eq!(state.last_disk_read, None);
    assert_eq!(state.disk_position, None);
}

#[test]
fn initialization_without_hooks_emits_periodic_and_final_error_progress() {
    let mut host = host();
    let mut seconds = 0;
    let mut telemetry = Telemetry::with_clock(Vec::new(), Duration::from_secs(1), move || {
        seconds += 1;
        Duration::from_secs(seconds)
    })
    .unwrap();
    let program = Program::parse("+[]", true, true).unwrap();
    let options = RunOptions {
        max_dispatches: Some(8193),
        ..options()
    };
    let mut output = Vec::new();
    let error = host
        .run_with_telemetry(&program, &options, &mut output, &mut telemetry)
        .unwrap_err();
    assert!(error.contains("dispatch limit"));
    assert!(output.is_empty());
    let text = String::from_utf8(telemetry.into_inner()).unwrap();
    assert!(text.contains("phase=cpu-init"));
    assert!(text.contains("bf=4096"));
    assert!(text.contains("bf=8192"));
    assert!(text.contains("bf=8193"));
    assert!(text.contains("guest=0"));
    assert!(text.contains("status=error: Brainfuck dispatch limit exceeded"));
}

#[test]
fn pc_is_published_only_at_validated_instruction_boundaries() {
    let mut source = Source::default();
    source.at(ORIGIN + 8 * 40);
    source.text.push('$');
    source.at(ORIGIN + 17);
    source.value(9); // Mid-instruction register state is not a published PC.
    source.store(HDR_TTY_OUT, 89);
    source.at(ORIGIN + 8 * 40);
    source.text.push('$'); // Invalid target must retain the last validated PC.
    let program = Program::parse(&source.text, true, true).unwrap();
    let mut host = host();
    let mut progress = Capture::default();
    let mut output = Vec::new();
    let error = host
        .run_with_telemetry(&program, &options(), &mut output, &mut progress)
        .unwrap_err();
    assert!(error.contains("outside its code image"));
    assert_eq!(output, b"Y");
    let final_state = progress.updates.last().unwrap().0;
    assert_eq!(final_state.pc, Some((0, 0)));
    assert_eq!(final_state.instructions, 1);
    assert_eq!(host.machine.tape()[ORIGIN + 17], 9);
    assert_eq!(final_state.last_store, Some(HDR_TTY_OUT));

    let mut invalid = self::host();
    let mut progress = Capture::default();
    assert!(invalid
        .run_with_telemetry(
            &Program::parse("$", true, true).unwrap(),
            &options(),
            &mut Vec::new(),
            &mut progress
        )
        .is_err());
    assert!(progress.updates.iter().all(|(state, _)| state.pc.is_none()));
}

#[test]
fn legacy_framebuffer_counts_only_actual_back_buffer_stores_and_keeps_stdout() {
    let mut source = Source::default();
    source.store(HDR_DISP_RESOLUTION, 0x0201);
    source.store(HDR_DISP_MODE, DISP_RGB565);
    source.store(34, 0xf800);
    source.text.push('!'); // A repeated store is another write, not another pixel.
    source.store(35, 31);
    source.store(32, 65535); // Front writes are ignored, not back writes.
    source.store(36, 42); // Adjacent ordinary RAM.
    source.store(64 + 34, 77); // Same offset in another bank.
    source.store(HDR_DISP_FLUSH, 1);
    source.store(HDR_TTY_OUT, 89);
    source.store(HDR_DISP_MODE, DISP_TTY);
    source.store(34, 123); // Mode changed: this is now ordinary RAM.
    let program = Program::parse(&source.text, true, true).unwrap();
    let mut actual = host();
    let mut plain = host();
    let mut progress = Capture::default();
    let mut output = Vec::new();
    let mut expected_output = Vec::new();
    let result = actual.run_with_telemetry(&program, &options(), &mut output, &mut progress);
    assert_eq!(
        result,
        plain.run(&program, &options(), &mut expected_output)
    );
    assert_eq!(output, b"Y");
    assert_eq!(output, expected_output);
    assert_eq!(actual.machine.tape(), plain.machine.tape());
    let final_state = progress.updates.last().unwrap().0;
    assert_eq!(final_state.frames, 1);
    assert_eq!(final_state.framebuffer_writes, 3);
    assert_eq!(final_state.last_framebuffer_store, Some(35));
    assert_eq!(final_state.last_store, Some(34));
}

#[test]
fn telemetry_observes_a_stepped_wait_without_requiring_a_timing_race() {
    let mut host = host();
    {
        let mut memory = StridedMemory::new(host.machine.tape_mut(), 256);
        memory.write(63, 1);
        memory.write(64, 8);
    }
    let mut source = Source::default();
    source.store(28, 63);
    source.store(29, 3);
    let (tx, rx) = mpsc::channel();
    let mut progress = Capture {
        step: Some(tx),
        ..Capture::default()
    };
    let options = RunOptions {
        clock: ClockPolicy::Stepped,
        control: Some(rx),
        ..options()
    };
    let program = Program::parse(&source.text, true, true).unwrap();
    assert_eq!(
        host.run_with_telemetry(&program, &options, &mut Vec::new(), &mut progress)
            .unwrap(),
        "halted"
    );
    assert!(progress
        .updates
        .iter()
        .any(|(state, _)| state.waiting_for_tick && state.tick == 0));
    let final_state = progress.updates.last().unwrap().0;
    assert_eq!(final_state.tick, 1);
    assert!(!final_state.waiting_for_tick);
}

#[test]
fn telemetry_output_failure_is_an_explicit_run_error() {
    struct Broken;
    impl io::Write for Broken {
        fn write(&mut self, _: &[u8]) -> io::Result<usize> {
            Err(io::Error::other("progress pipe closed"))
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }
    let mut progress = Telemetry::new(Broken, Duration::from_secs(1)).unwrap();
    let program = Program::parse("+", true, true).unwrap();
    let mut host = host();
    let error = host
        .run_with_telemetry(&program, &options(), &mut Vec::new(), &mut progress)
        .unwrap_err();
    assert!(error.contains("Telemetry output"));
    assert!(error.contains("progress pipe closed"));
    assert_eq!(host.machine.dispatches(), 0);
}
