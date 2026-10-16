//! Host policies around the unchanged guest instruction loop.
use crate::{
    capture,
    devices::{key_code, InputTransition},
    vm::{VMState, VM},
};
use clap::ValueEnum;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{BufRead, Read},
    path::{Path, PathBuf},
    sync::mpsc::{self, Receiver, TryRecvError},
    thread,
    time::{Duration, Instant},
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, ValueEnum, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ClockPolicy {
    #[default]
    Unthrottled,
    Paced,
    Stepped,
}

#[derive(Default)]
pub struct RunOptions {
    pub clock: ClockPolicy,
    pub screenshot: Option<PathBuf>,
    pub raw_frame: Option<PathBuf>,
    pub expect_frame: Option<PathBuf>,
    pub capture_frame: u64,
    pub max_instructions: Option<u64>,
    pub max_ticks: Option<u64>,
    pub timeout: Option<Duration>,
    pub frequency: Option<u64>,
    pub deterministic: bool,
    pub control: Option<Receiver<Result<ControlCommand, String>>>,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
pub enum ControlCommand {
    Step { ticks: u64 },
    Key { key: String, down: bool },
    Input { event: InputTransition },
    Quit,
}

/// Stdin is reserved for host commands, never also consumed as guest TTY input.
pub fn stdin_control() -> Receiver<Result<ControlCommand, String>> {
    let (sender, receiver) = mpsc::sync_channel(128);
    thread::spawn(move || {
        for line in std::io::stdin().lock().lines() {
            let command = line.map_err(|e| e.to_string()).and_then(|line| {
                serde_json::from_str(&line).map_err(|e| format!("Invalid control command: {e}"))
            });
            if sender.send(command).is_err() {
                break;
            }
        }
    });
    receiver
}

#[derive(Serialize)]
pub struct RunReport {
    pub report_version: u16,
    pub success: bool,
    pub stop_reason: String,
    pub instructions: u64,
    pub tick: u64,
    pub milliseconds: u32,
    pub tick_hz: u16,
    pub clock: ClockPolicy,
    pub frame: u64,
    pub width: Option<u16>,
    pub height: Option<u16>,
    pub bank_size: u16,
    pub memory_cells: usize,
    pub seed: u32,
    pub binary_sha256: String,
    pub disk_sha256: Option<String>,
    pub input_script_sha256: Option<String>,
    pub input_overflow: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub jit: Option<crate::vm::JitStats>,
}

pub fn sha256_file(path: &Path) -> Result<String, String> {
    let mut file = fs::File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 65536];
    loop {
        let count = file.read(&mut buffer).map_err(|e| e.to_string())?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(format!("{:x}", hasher.finalize()))
}

impl RunReport {
    pub fn finish(&mut self, vm: &VM, result: &Result<String, String>) {
        self.jit = vm.jit_stats();
        self.success = result.is_ok();
        self.stop_reason = match result {
            Ok(s) | Err(s) => s.clone(),
        };
        self.instructions = vm.executed_instructions;
        self.tick = vm.devices.tick();
        self.milliseconds = vm.devices.millis();
        self.frame = vm.presented_frames;
        self.input_overflow = vm.devices.overflow_seen();
        if let Some(frame) = vm.rgb565_display.as_ref().and_then(|d| d.snapshot()) {
            self.width = Some(frame.width);
            self.height = Some(frame.height);
        }
    }
    pub fn write(&self, path: &Path) -> Result<(), String> {
        if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
            fs::create_dir_all(parent).map_err(|e| format!("Creating report directory: {e}"))?;
        }
        fs::write(
            path,
            serde_json::to_vec_pretty(self).map_err(|e| e.to_string())?,
        )
        .map_err(|e| format!("Writing report {}: {e}", path.display()))
    }
}

/// Ordered host commands, shared by the native CPU and Brainfuck runners.
#[derive(Default)]
pub struct Controller {
    pub credits: u64,
    pub events: Vec<InputTransition>,
    pub closed: bool,
    pub quit: bool,
}
impl Controller {
    pub fn poll(
        &mut self,
        receiver: &Option<Receiver<Result<ControlCommand, String>>>,
    ) -> Result<(), String> {
        // A step command is a barrier: later key commands apply only after
        // its granted ticks, so a piped press/step/release sequence is replayable.
        if self.credits != 0 {
            return Ok(());
        }
        if let Some(receiver) = receiver {
            // Bound host work per iteration, even if a controller floods commands.
            for _ in 0..256 {
                match receiver.try_recv() {
                    Ok(command) => match command? {
                        ControlCommand::Step { ticks } => {
                            if ticks == 0 {
                                return Err("Step ticks must be positive".into());
                            }
                            self.credits = ticks;
                            break;
                        }
                        ControlCommand::Key { key, down } => {
                            let key = key_code(&key).ok_or_else(|| format!("Unknown key {key}"))?;
                            if self.events.len() >= 4096 {
                                return Err("Controller input queue overflow".into());
                            }
                            self.events.push(InputTransition::Key { key, down });
                        }
                        ControlCommand::Input { event } => {
                            if !event.valid() {
                                return Err("Invalid input code".into());
                            }
                            if self.events.len() >= 4096 {
                                return Err("Controller input queue overflow".into());
                            }
                            self.events.push(event);
                        }
                        ControlCommand::Quit => self.quit = true,
                    },
                    Err(TryRecvError::Empty) => break,
                    Err(TryRecvError::Disconnected) => {
                        self.closed = true;
                        break;
                    }
                }
            }
        }
        Ok(())
    }
}

pub fn run(vm: &mut VM, options: &RunOptions) -> Result<String, String> {
    let result = run_with_execution(vm, options, true);
    vm.devices.audio.finish()?;
    result
}

/// The original instruction-at-a-time runner, retained as an execution oracle
/// and for A/B measurements with the same guest binary and host policies.
pub fn run_reference(vm: &mut VM, options: &RunOptions) -> Result<String, String> {
    let result = run_with_execution(vm, options, false);
    vm.devices.audio.finish()?;
    result
}

fn run_with_execution(vm: &mut VM, options: &RunOptions, batched: bool) -> Result<String, String> {
    let capture_requested = options.screenshot.is_some() || options.raw_frame.is_some();
    if capture_requested && options.capture_frame == 0 {
        return Err("Capture frame must be at least 1".into());
    }
    if options.clock == ClockPolicy::Stepped && options.control.is_none() {
        return Err("Stepped clock requires --control-stdin".into());
    }
    if options.frequency == Some(0) {
        return Err("Instruction frequency must be positive".into());
    }
    if let Some(expected) = options.expect_frame.as_ref() {
        let baseline = expected
            .canonicalize()
            .map_err(|e| format!("Reading baseline {}: {e}", expected.display()))?;
        for output in [&options.screenshot, &options.raw_frame]
            .into_iter()
            .flatten()
        {
            if output.canonicalize().is_ok_and(|path| path == baseline) {
                return Err("Capture output must not overwrite the expected baseline".into());
            }
        }
    }
    let started = Instant::now();
    let tick_period = Duration::from_secs_f64(1.0 / f64::from(vm.devices.tick_hz()));
    let mut next_tick = started + tick_period;
    let mut frequency_epoch = started;
    let mut frequency_count = 0u64;
    let mut controller = Controller {
        credits: 0,
        events: Vec::new(),
        closed: false,
        quit: false,
    };
    let check_host = |vm: &mut VM| -> Result<bool, String> {
        if options
            .timeout
            .is_some_and(|limit| started.elapsed() >= limit)
        {
            return Err("Wall-time limit exceeded".into());
        }
        Ok(vm.poll_host_terminal() || vm.window_closed())
    };
    loop {
        if let Some(error) = vm.devices.audio.error() {
            return Err(format!("Audio output: {error}"));
        }
        if let Some(error) = vm.device_error.as_ref() {
            return Err(error.clone());
        }
        if options.deterministic && vm.devices.overflow_seen() {
            return Err("Input queue overflow during deterministic execution".into());
        }
        if capture_requested && vm.presented_frames >= options.capture_frame {
            let frame = vm
                .rgb565_display
                .as_ref()
                .and_then(|d| d.snapshot())
                .ok_or("No presented RGB565 frame")?;
            capture::capture_frame(
                &frame,
                options.screenshot.as_deref(),
                options.raw_frame.as_deref(),
            )?;
            if let Some(expected) = options.expect_frame.as_ref() {
                let artifacts = options
                    .screenshot
                    .as_ref()
                    .or(options.raw_frame.as_ref())
                    .ok_or("Comparison needs a capture output path")?
                    .with_extension("diff");
                if !capture::compare_raw(&frame, expected, &artifacts)? {
                    return Err(format!(
                        "Visual snapshot mismatch; artifacts: {}",
                        artifacts.display()
                    ));
                }
            }
            return Ok("captured_frame".into());
        }
        if !matches!(vm.state, VMState::Running) {
            if capture_requested {
                return Err(format!(
                    "Guest stopped before frame {}",
                    options.capture_frame
                ));
            }
            return match &vm.state {
                VMState::Halted => Ok("halted".into()),
                VMState::Error(error) => Err(error.clone()),
                _ => Err("Guest stopped without halting".into()),
            };
        }
        if options
            .max_instructions
            .is_some_and(|limit| vm.executed_instructions >= limit)
        {
            return Err("Instruction limit exceeded".into());
        }
        if vm.executed_instructions % 1024 == 0 || vm.devices.has_pending_tick() {
            controller.poll(&options.control)?;
            if controller.quit || check_host(vm)? {
                return if capture_requested {
                    Err("Stopped before requested frame".into())
                } else {
                    Ok("host_stop".into())
                };
            }
        }
        if vm.devices.has_pending_tick() {
            if options
                .max_ticks
                .is_some_and(|limit| vm.devices.tick() >= limit)
            {
                return if capture_requested {
                    Err("Tick limit reached before requested frame".into())
                } else {
                    Ok("tick_limit".into())
                };
            }
            loop {
                let waiting = match options.clock {
                    ClockPolicy::Unthrottled => false,
                    ClockPolicy::Paced => Instant::now() < next_tick,
                    ClockPolicy::Stepped => controller.credits == 0,
                };
                if !waiting {
                    break;
                }
                controller.poll(&options.control)?;
                if controller.quit || check_host(vm)? {
                    return if capture_requested {
                        Err("Stopped before requested frame".into())
                    } else {
                        Ok("host_stop".into())
                    };
                }
                if options.clock == ClockPolicy::Stepped
                    && controller.closed
                    && controller.credits == 0
                {
                    return Err(
                        "Step controller closed while the guest was waiting for a tick".into(),
                    );
                }
                thread::sleep(Duration::from_millis(1));
            }
            if options.clock == ClockPolicy::Stepped {
                controller.credits -= 1;
            }
            vm.complete_tick(std::mem::take(&mut controller.events))?;
            // Late execution accrues no debt: never grant catch-up ticks from wall time.
            next_tick = Instant::now() + tick_period;
            continue;
        }
        if batched && !vm.debug_mode && !vm.verbose && options.frequency.is_none() {
            // The interpreter polls every 1024 instructions. A compiled guest
            // stays native across that gap; header stores, halt, and ticks
            // still end the batch. Quit and window checks run on the coarser
            // cadence, which is well under a frame.
            let quantum = if vm.jit_enabled { 65_536 } else { 1024 };
            let mut budget = quantum - vm.executed_instructions % quantum;
            if let Some(limit) = options.max_instructions {
                budget = budget.min(limit - vm.executed_instructions);
            }
            vm.run_batch(budget)?;
        } else {
            vm.step()?;
        }
        if let Some(frequency) = options.frequency {
            frequency_count += 1;
            let batch = (frequency / 100).max(1);
            if frequency_count >= batch {
                let deadline = frequency_epoch
                    + Duration::from_secs_f64(frequency_count as f64 / frequency as f64);
                while Instant::now() < deadline {
                    if check_host(vm)? {
                        return Err("Window closed while throttling".into());
                    }
                    thread::sleep(
                        deadline
                            .saturating_duration_since(Instant::now())
                            .min(Duration::from_millis(10)),
                    );
                }
                frequency_epoch = Instant::now();
                frequency_count = 0;
            }
        }
    }
}
