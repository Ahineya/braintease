use crate::{
    devices::BfDevices,
    image::{Layout, StridedMemory},
    telemetry::{Progress, Snapshot},
};
use bf_wasm::engine::{Machine, Program, Stop};
use rvm::{
    capture,
    constants::{DISP_RGB565, HDR_DISP_MODE},
    devices::GuestMemory,
    runner::{ClockPolicy, ControlCommand, Controller},
};
use serde::Serialize;
use std::{
    io::Write,
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc::Receiver,
        Arc,
    },
    thread,
    time::{Duration, Instant},
};

pub struct RunOptions {
    pub clock: ClockPolicy,
    pub screenshot: Option<PathBuf>,
    pub raw_frame: Option<PathBuf>,
    pub text_frame: Option<PathBuf>,
    pub expect_frame: Option<PathBuf>,
    pub capture_frame: u64,
    pub max_dispatches: Option<u64>,
    pub max_instructions: Option<u64>,
    pub max_ticks: Option<u64>,
    pub timeout: Option<Duration>,
    pub deterministic: bool,
    pub control: Option<Receiver<Result<ControlCommand, String>>>,
    pub tty_input: Option<Receiver<Result<Vec<u8>, String>>>,
    pub cancelled: Arc<AtomicBool>,
}

impl Default for RunOptions {
    fn default() -> Self {
        Self {
            clock: ClockPolicy::Unthrottled,
            screenshot: None,
            raw_frame: None,
            text_frame: None,
            expect_frame: None,
            capture_frame: 1,
            max_dispatches: None,
            max_instructions: None,
            max_ticks: None,
            timeout: None,
            deterministic: false,
            control: None,
            tty_input: None,
            cancelled: Arc::new(AtomicBool::new(false)),
        }
    }
}

#[derive(Serialize)]
pub struct RunReport {
    pub report_version: u16,
    pub backend: &'static str,
    pub success: bool,
    pub stop_reason: String,
    /// Optimized interpreter dispatches, not literal BF characters.
    pub bf_dispatches: u64,
    /// Observed starts of guest instructions; the host never executes them.
    pub ripple_instructions: u64,
    pub tick: u64,
    pub milliseconds: u32,
    pub tick_hz: u16,
    pub clock: ClockPolicy,
    pub frames: u64,
    pub width: Option<u16>,
    pub height: Option<u16>,
    pub elapsed_seconds: f64,
    pub layout: Layout,
    pub seed: u32,
    pub source_sha256: String,
    pub disk_sha256: Option<String>,
    pub input_script_sha256: Option<String>,
    pub input_overflow: bool,
    pub guest_result: u16,
}

impl RunReport {
    pub fn write(&self, path: &Path) -> Result<(), String> {
        if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
            std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        }
        std::fs::write(
            path,
            serde_json::to_vec_pretty(self).map_err(|e| e.to_string())?,
        )
        .map_err(|e| format!("Writing report {}: {e}", path.display()))
    }
}

pub struct Host {
    pub layout: Layout,
    pub machine: Machine,
    pub devices: BfDevices,
    pub instructions: u64,
}

#[derive(Default)]
struct Observation {
    pc: Option<(u16, u16)>,
    last_load: Option<usize>,
    last_store: Option<usize>,
    legacy_back: Option<std::ops::Range<usize>>,
    framebuffer_writes: u64,
    last_framebuffer_store: Option<usize>,
    waiting: bool,
}

impl Observation {
    fn display_mode(&mut self, host: &Host) {
        self.legacy_back = if host.devices.display_mode() == DISP_RGB565 {
            let shared = host.devices.display.get_state();
            let state = shared.lock().unwrap();
            let pixels = usize::from(state.width) * usize::from(state.height);
            Some(32 + pixels..32 + 2 * pixels)
        } else {
            None
        };
    }

    fn snapshot(&self, host: &Host, options: &RunOptions) -> Snapshot {
        Snapshot {
            dispatches: host.machine.dispatches(),
            instructions: host.instructions,
            pc: self.pc,
            frames: host.devices.presented_frames,
            tick: host.devices.devices.tick(),
            clock: options.clock,
            waiting_for_tick: self.waiting,
            last_load: self.last_load,
            last_store: self.last_store,
            framebuffer_writes: self.framebuffer_writes,
            last_framebuffer_store: self.last_framebuffer_store,
            bank_size: host.layout.bank_size,
        }
    }
}

impl Host {
    pub fn new(layout: Layout, devices: BfDevices) -> Result<Self, String> {
        layout.validate(layout.tape_cells)?;
        if layout.bank_size != devices.bank_size() {
            return Err("CPU and device bank sizes must match".into());
        }
        let machine = Machine::new(layout.tape_cells).map_err(|e| e.to_string())?;
        Ok(Self {
            layout,
            machine,
            devices,
            instructions: 0,
        })
    }

    pub fn run(
        &mut self,
        program: &Program,
        options: &RunOptions,
        output: &mut impl Write,
    ) -> Result<String, String> {
        self.run_observed(program, options, output, None)
    }

    /// Progress has its own writer; guest output and existing callers are unchanged.
    pub fn run_with_telemetry(
        &mut self,
        program: &Program,
        options: &RunOptions,
        output: &mut impl Write,
        progress: &mut dyn Progress,
    ) -> Result<String, String> {
        self.run_observed(program, options, output, Some(progress))
    }

    fn run_observed(
        &mut self,
        program: &Program,
        options: &RunOptions,
        output: &mut impl Write,
        mut progress: Option<&mut dyn Progress>,
    ) -> Result<String, String> {
        let mut observation = progress.as_ref().map(|_| {
            let mut observation = Observation::default();
            observation.display_mode(self);
            observation
        });
        let result = if let (Some(progress), Some(observation)) =
            (progress.as_deref_mut(), observation.as_ref())
        {
            progress.update(observation.snapshot(self, options), Some("started"))
        } else {
            Ok(())
        };
        let result = result
            .and_then(|()| self.execute(program, options, output, &mut progress, &mut observation));
        let bytes = self.devices.take_output();
        let write_result = output
            .write_all(&bytes)
            .and_then(|()| output.flush())
            .map_err(|e| format!("Guest output: {e}"));
        let disk_result = self.devices.flush_storage();
        let result = result.and_then(|reason| {
            write_result?;
            disk_result?;
            Ok(reason)
        });
        if let (Some(progress), Some(observation)) = (progress, observation) {
            let status = match &result {
                Ok(reason) => reason.clone(),
                Err(reason) => format!("error: {reason}"),
            };
            if let Err(error) = progress.update(observation.snapshot(self, options), Some(&status))
            {
                return Err(match result {
                    Err(original) => format!("{original}; {error}"),
                    Ok(_) => error,
                });
            }
        }
        result
    }

    fn execute(
        &mut self,
        program: &Program,
        options: &RunOptions,
        output: &mut impl Write,
        progress: &mut Option<&mut dyn Progress>,
        observation: &mut Option<Observation>,
    ) -> Result<String, String> {
        let rgb_capture = options.screenshot.is_some() || options.raw_frame.is_some();
        let capture = rgb_capture || options.text_frame.is_some();
        if options.text_frame.is_some() && (rgb_capture || options.expect_frame.is_some()) {
            return Err(
                "TEXT40 capture cannot be combined with RGB565 capture or comparison".into(),
            );
        }
        if capture && options.capture_frame == 0 {
            return Err("Capture frame must be positive".into());
        }
        if options.expect_frame.is_some() && !rgb_capture {
            return Err("--expect-frame requires --screenshot or --raw-frame".into());
        }
        if options.clock == ClockPolicy::Stepped && options.control.is_none() {
            return Err("Stepped clock requires --control-stdin".into());
        }
        if let Some(expected) = &options.expect_frame {
            let baseline = expected.canonicalize().map_err(|e| e.to_string())?;
            for path in [&options.screenshot, &options.raw_frame]
                .into_iter()
                .flatten()
            {
                if path.canonicalize().is_ok_and(|p| p == baseline) {
                    return Err("Capture must not overwrite its baseline".into());
                }
            }
        }
        let started = Instant::now();
        let tick_period = Duration::from_secs_f64(1.0 / f64::from(self.devices.devices.tick_hz()));
        let mut next_tick = started + tick_period;
        let mut controller = Controller::default();
        let mut halted = false;
        let mut poll_at = 0;
        loop {
            // CPU and scan work are bounded by dispatch budgets, including before
            // the guest has initialized its tape or reached any device hook.
            if self.machine.dispatches() >= poll_at || self.devices.devices.has_pending_tick() {
                poll_at = self.machine.dispatches().saturating_add(4096);
                self.poll_progress(options, progress, observation, false)?;
                self.check_host(options, started)?;
                controller.poll(&options.control)?;
                self.devices.poll_host_input()?;
                if controller.quit
                    || options.cancelled.load(Ordering::Relaxed)
                    || self.devices.window_closed()
                {
                    return if capture {
                        Err("Stopped before requested frame".into())
                    } else {
                        Ok("host_stop".into())
                    };
                }
                if let Some(receiver) = &options.tty_input {
                    for _ in 0..64 {
                        match receiver.try_recv() {
                            Ok(bytes) => self.devices.push_input(&bytes?),
                            Err(_) => break,
                        }
                    }
                }
                let bytes = self.devices.take_output();
                if !bytes.is_empty() {
                    output
                        .write_all(&bytes)
                        .and_then(|()| output.flush())
                        .map_err(|e| e.to_string())?;
                }
            }
            if options.deterministic && self.devices.devices.overflow_seen() {
                return Err("Input overflow during deterministic execution".into());
            }
            if capture && self.devices.presented_frames >= options.capture_frame {
                if let Some(path) = &options.text_frame {
                    let frame = self
                        .devices
                        .text_display
                        .snapshot()
                        .filter(|frame| frame.sequence == self.devices.presented_frames)
                        .ok_or("Requested frame is not a presented TEXT40 frame")?;
                    frame.write_raw(path)?;
                    return Ok("captured_frame".into());
                }
                let frame = self
                    .devices
                    .display
                    .snapshot()
                    .ok_or("No presented RGB565 frame")?;
                capture::capture_frame(
                    &frame,
                    options.screenshot.as_deref(),
                    options.raw_frame.as_deref(),
                )?;
                if let Some(expected) = &options.expect_frame {
                    let artifacts = options
                        .screenshot
                        .as_ref()
                        .or(options.raw_frame.as_ref())
                        .unwrap()
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
            if halted {
                return if capture {
                    Err(format!(
                        "Guest halted before frame {}",
                        options.capture_frame
                    ))
                } else {
                    Ok("halted".into())
                };
            }
            if self.devices.devices.has_pending_tick() {
                if options
                    .max_ticks
                    .is_some_and(|limit| self.devices.devices.tick() >= limit)
                {
                    return if capture {
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
                    self.poll_progress(options, progress, observation, true)?;
                    self.check_host(options, started)?;
                    controller.poll(&options.control)?;
                    self.devices.poll_host_input()?;
                    if controller.quit
                        || options.cancelled.load(Ordering::Relaxed)
                        || self.devices.window_closed()
                    {
                        return if capture {
                            Err("Stopped before requested frame".into())
                        } else {
                            Ok("host_stop".into())
                        };
                    }
                    if options.clock == ClockPolicy::Stepped
                        && controller.closed
                        && controller.credits == 0
                    {
                        return Err("Step controller closed while waiting for a tick".into());
                    }
                    thread::sleep(Duration::from_millis(1));
                }
                if options.clock == ClockPolicy::Stepped {
                    controller.credits -= 1;
                }
                let mut memory =
                    StridedMemory::new(self.machine.tape_mut(), self.layout.memory_cells());
                self.devices
                    .complete_tick(&mut memory, std::mem::take(&mut controller.events))?;
                if let Some(observation) = observation {
                    observation.waiting = false;
                }
                // A slow BF guest accrues no wall-clock debt or catch-up ticks.
                next_tick = Instant::now() + tick_period;
                continue;
            }
            let mut budget = 4096;
            if let Some(limit) = options.max_dispatches {
                if self.machine.dispatches() >= limit {
                    return Err("Brainfuck dispatch limit exceeded".into());
                }
                budget = budget.min(limit - self.machine.dispatches());
            }
            match self.machine.run(program, budget).map_err(|e| e.to_string())? {
                Stop::Halted => halted = true,
                Stop::Budget => {},
                Stop::Input => return Err("Unexpected BF input opcode in Ripple host image".into()),
                Stop::Output(_) => return Err("Unexpected BF output opcode; rebuild with --bf-host to avoid duplicate TTY output".into()),
                Stop::Hook(b'$') => {
                    if options.max_instructions.is_some_and(|limit| self.instructions >= limit) { return Err("Ripple instruction limit exceeded".into()); }
                    let pc = usize::from(self.machine.tape()[self.layout.origin + 17]);
                    let bank = usize::from(self.machine.tape()[self.layout.origin + 25]);
                    // Inspect published CPU state, never decode or execute an
                    // instruction. Raw jump/return offsets may exceed a bank;
                    // RVM fetch uses the same flattened address. I is the
                    // template's implicit final HALT.
                    let index = bank * usize::from(self.layout.bank_size) + pc;
                    if index > self.layout.instructions {
                        return Err(format!("BF CPU target {bank}:{pc} is outside its code image"));
                    }
                    if self.machine.pointer() != self.layout.origin + 8 * (40 + 4 * index) {
                        return Err("Instruction hook does not match the declared BF CPU layout".into());
                    }
                    self.instructions = self.instructions.checked_add(1).ok_or("Instruction counter overflow")?;
                    if let Some(observation) = observation {
                        observation.pc = Some((bank as u16, pc as u16));
                    }
                },
                Stop::Hook(kind @ (b'?' | b'!')) => {
                    let address = self.layout.ram_address(self.machine.pointer())?;
                    if let Some(observation) = observation {
                        if kind == b'?' { observation.last_load = Some(address); }
                        else { observation.last_store = Some(address); }
                    }
                    let mut memory = StridedMemory::new(self.machine.tape_mut(), self.layout.memory_cells());
                    if kind == b'?' {
                        let value = self.devices.read_mmio(address, &mut memory)?;
                        memory.write(address, value);
                    } else {
                        let value = memory.read(address);
                        self.devices.write_mmio(address, value, &mut memory)?;
                    }
                    if kind == b'!' {
                        if let Some(observation) = observation {
                            if address == HDR_DISP_MODE { observation.display_mode(self); }
                            if observation.legacy_back.as_ref().is_some_and(|range| range.contains(&address)) {
                                observation.framebuffer_writes = observation.framebuffer_writes.checked_add(1).ok_or("Telemetry framebuffer counter overflow")?;
                                observation.last_framebuffer_store = Some(address);
                            }
                        }
                    }
                },
                Stop::Hook(other) => return Err(format!("Unknown BF host hook {other}")),
            }
        }
    }

    fn poll_progress(
        &self,
        options: &RunOptions,
        progress: &mut Option<&mut dyn Progress>,
        observation: &mut Option<Observation>,
        waiting: bool,
    ) -> Result<(), String> {
        if let (Some(progress), Some(observation)) = (progress.as_deref_mut(), observation.as_mut())
        {
            observation.waiting = waiting;
            progress.update(observation.snapshot(self, options), None)?;
        }
        Ok(())
    }

    fn check_host(&self, options: &RunOptions, started: Instant) -> Result<(), String> {
        if options
            .timeout
            .is_some_and(|limit| started.elapsed() >= limit)
        {
            return Err("Wall-time limit exceeded".into());
        }
        Ok(())
    }
}
