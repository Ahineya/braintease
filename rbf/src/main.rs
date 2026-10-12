use bf_wasm::engine::Program;
use clap::Parser;
use rbf::{
    devices::BfDevices,
    image::Layout,
    runner::{Host, RunOptions, RunReport},
    telemetry::{parse_interval, Telemetry},
};
use rvm::{
    devices::InputScript,
    display_rgb565,
    runner::{self, ClockPolicy},
    vm::Storage,
};
use std::{
    fs,
    io::{self, Read},
    path::PathBuf,
    sync::{atomic::AtomicBool, mpsc, Arc},
    thread,
    time::{Duration, Instant},
};

#[derive(Parser)]
#[command(about = "Optimized Brainfuck host for Ripple CPU exports (16-bit wrapping cells)")]
struct Cli {
    /// Expanded output from rlink --bf-host followed by bfm expand
    program: PathBuf,
    /// Open the RGB565 window with live keyboard input
    #[arg(long, conflicts_with_all = ["headless", "terminal"])]
    visual: bool,
    /// Render TEXT40 in the terminal, with live arrows/Z/X and TTY input
    #[arg(long, conflicts_with_all = ["visual", "headless", "tty_stdin", "control_stdin"])]
    terminal: bool,
    /// Deterministic execution without a window or live input (default)
    #[arg(long)]
    headless: bool,
    /// Explicit writable block disk; no default host disk is opened
    #[arg(long)]
    disk: Option<PathBuf>,
    #[arg(long, default_value_t = 0x12345678)]
    seed: u32,
    #[arg(long, default_value_t = 35, value_parser = clap::value_parser!(u16).range(1..))]
    tick_hz: u16,
    #[arg(long, value_enum, default_value_t = ClockPolicy::Unthrottled)]
    clock: ClockPolicy,
    #[arg(long)]
    input_script: Option<PathBuf>,
    #[arg(long)]
    control_stdin: bool,
    /// Read guest TTY bytes from stdin; unavailable with the JSON controller
    #[arg(long, conflicts_with = "control_stdin")]
    tty_stdin: bool,
    /// Queue literal UTF-8 bytes for the guest TTY
    #[arg(short = 'i', long)]
    input: Option<String>,
    #[arg(long)]
    screenshot: Option<PathBuf>,
    #[arg(long)]
    raw_frame: Option<PathBuf>,
    /// Capture 1000 little-endian TEXT40 character/attribute words
    #[arg(long, conflicts_with_all = ["screenshot", "raw_frame", "expect_frame"])]
    text_frame: Option<PathBuf>,
    #[arg(long)]
    expect_frame: Option<PathBuf>,
    #[arg(long, default_value_t = 1)]
    capture_frame: u64,
    #[arg(long)]
    report: Option<PathBuf>,
    /// Print periodic progress to stderr without changing guest stdout
    #[arg(long, conflicts_with = "terminal")]
    telemetry: bool,
    /// Progress interval in seconds (0.01..3600; default 1)
    #[arg(long, requires = "telemetry", value_parser = parse_interval)]
    telemetry_interval: Option<Duration>,
    /// Limit optimized BF dispatches (not literal BF characters)
    #[arg(long)]
    max_dispatches: Option<u64>,
    /// Limit observed Ripple instruction starts; CPU work still runs in BF
    #[arg(long)]
    max_instructions: Option<u64>,
    #[arg(long)]
    max_ticks: Option<u64>,
    /// Wall-time limit in seconds, including guest tape initialization
    #[arg(long)]
    timeout: Option<u64>,
    /// Upper bound for the tape allocation declared by the image
    #[arg(long, default_value_t = 64_000_000)]
    max_tape_cells: usize,
    /// Disable affine-loop and offset-block optimization for comparison
    #[arg(long)]
    no_optimize: bool,
    /// Return the low octet of guest RV0 after a normal HALT
    #[arg(long)]
    exit_code: bool,
}

fn main() {
    match execute(Cli::parse()) {
        Ok(code) => std::process::exit(code),
        Err(error) => {
            eprintln!("rbf: {error}");
            std::process::exit(1);
        }
    }
}

fn deterministic_input(cli: &Cli) -> bool {
    (!cli.visual && !cli.terminal)
        || cli.input_script.is_some()
        || cli.control_stdin
        || cli.screenshot.is_some()
        || cli.raw_frame.is_some()
        || cli.text_frame.is_some()
}

fn execute(cli: Cli) -> Result<i32, String> {
    let deterministic = deterministic_input(&cli);
    if (cli.clock == ClockPolicy::Stepped) != cli.control_stdin {
        return Err("--clock stepped and --control-stdin must be used together".into());
    }
    let mut telemetry = if cli.telemetry {
        Some(Telemetry::new(
            io::stderr(),
            cli.telemetry_interval.unwrap_or(Duration::from_secs(1)),
        )?)
    } else {
        None
    };
    if let Some(telemetry) = &mut telemetry {
        telemetry.phase("loading", &format!("program={}", cli.program.display()))?;
    }
    let source =
        fs::read_to_string(&cli.program).map_err(|e| format!("{}: {e}", cli.program.display()))?;
    let (layout, body) = Layout::parse(&source, cli.max_tape_cells)?;
    if let Some(telemetry) = &mut telemetry {
        telemetry.phase(
            "parsing",
            &format!(
                "source_bytes={} tape_cells={} guest_instructions={}",
                source.len(),
                layout.tape_cells,
                layout.instructions
            ),
        )?;
    }
    let program = Program::parse(body, !cli.no_optimize, true).map_err(|e| e.to_string())?;
    let disk_hash = cli
        .disk
        .as_deref()
        .filter(|path| path.exists())
        .map(runner::sha256_file)
        .transpose()?;
    let storage = cli
        .disk
        .map(|path| Storage::with_path(path).map_err(|e| e.to_string()))
        .transpose()?;
    let mut devices = BfDevices::new(layout.bank_size, cli.tick_hz, cli.seed, storage)?;
    if let Some(path) = &cli.input_script {
        devices.devices.set_script(InputScript::from_path(path)?)?;
    }
    if let Some(text) = &cli.input {
        devices.push_input(text.as_bytes());
    }
    devices.host_input_enabled = !deterministic;
    devices
        .text_display
        .set_interactive(cli.terminal && !deterministic);
    if cli.terminal && !deterministic {
        rvm::vm::install_terminal_cleanup_hook();
    }
    let cancelled = Arc::new(AtomicBool::new(false));
    for signal in [signal_hook::consts::SIGINT, signal_hook::consts::SIGTERM] {
        signal_hook::flag::register(signal, Arc::clone(&cancelled)).map_err(|e| e.to_string())?;
    }
    let options = RunOptions {
        clock: cli.clock,
        screenshot: cli.screenshot,
        raw_frame: cli.raw_frame,
        text_frame: cli.text_frame,
        expect_frame: cli.expect_frame,
        capture_frame: cli.capture_frame,
        max_dispatches: cli.max_dispatches,
        max_instructions: cli.max_instructions,
        max_ticks: cli.max_ticks,
        timeout: cli.timeout.map(Duration::from_secs),
        deterministic,
        control: cli.control_stdin.then(runner::stdin_control),
        tty_input: cli.tty_stdin.then(stdin_bytes),
        cancelled,
    };
    let mut host = Host::new(layout.clone(), devices)?;
    if let Some(telemetry) = &mut telemetry {
        telemetry.phase(
            "ready",
            &format!(
                "bf_ir={} optimized_regions={}",
                program.instruction_count(),
                program.optimization_count()
            ),
        )?;
    }
    let state = host.devices.display.get_state();
    let mut report = RunReport {
        report_version: 1,
        backend: "brainfuck",
        success: false,
        stop_reason: String::new(),
        bf_dispatches: 0,
        ripple_instructions: 0,
        tick: 0,
        milliseconds: 0,
        tick_hz: cli.tick_hz,
        clock: cli.clock,
        frames: 0,
        width: None,
        height: None,
        elapsed_seconds: 0.0,
        layout,
        seed: cli.seed,
        source_sha256: runner::sha256_file(&cli.program)?,
        disk_sha256: disk_hash,
        input_script_sha256: cli
            .input_script
            .as_deref()
            .map(runner::sha256_file)
            .transpose()?,
        input_overflow: false,
        guest_result: 0,
    };
    let worker_state = Arc::clone(&state);
    let work = move || {
        let start = Instant::now();
        let result = if let Some(telemetry) = &mut telemetry {
            host.run_with_telemetry(&program, &options, &mut io::stdout().lock(), telemetry)
        } else {
            host.run(&program, &options, &mut io::stdout().lock())
        };
        report.elapsed_seconds = start.elapsed().as_secs_f64();
        report.bf_dispatches = host.machine.dispatches();
        report.ripple_instructions = host.instructions;
        report.tick = host.devices.devices.tick();
        report.milliseconds = host.devices.devices.millis();
        report.frames = host.devices.presented_frames;
        report.input_overflow = host.devices.devices.overflow_seen();
        report.guest_result = host.machine.tape()[host.layout.origin + 49]; // Rv0 = R5, register lane = 1.
        if let Some(frame) = host.devices.display.snapshot() {
            report.width = Some(frame.width);
            report.height = Some(frame.height);
        } else if host.devices.text_display.snapshot().is_some() {
            report.width = Some(40);
            report.height = Some(25);
        }
        host.devices.text_display.leave();
        worker_state.lock().unwrap().request_shutdown();
        (report, result)
    };
    let (mut report, result) = if cli.visual {
        let worker = thread::spawn(work);
        let window =
            display_rgb565::run_rgb565_display(Arc::clone(&state)).map_err(|e| e.to_string());
        state.lock().unwrap().request_shutdown();
        let (report, result) = worker.join().map_err(|_| "Brainfuck worker panicked")?;
        // A window/GPU failure is a failed run even if shutdown made the guest
        // worker return an otherwise successful host_stop.
        (report, window.and(result))
    } else {
        work()
    };
    report.success = result.is_ok();
    report.stop_reason = match &result {
        Ok(reason) | Err(reason) => reason.clone(),
    };
    if let Some(path) = cli.report {
        report.write(&path)?;
    }
    let reason = result?;
    Ok(if cli.exit_code && reason == "halted" {
        i32::from(report.guest_result & 255)
    } else {
        0
    })
}

fn stdin_bytes() -> mpsc::Receiver<Result<Vec<u8>, String>> {
    let (tx, rx) = mpsc::sync_channel(64);
    thread::spawn(move || {
        let mut input = io::stdin().lock();
        let mut buffer = [0; 256];
        loop {
            match input.read(&mut buffer) {
                Ok(0) => break,
                Ok(count) => {
                    if tx.send(Ok(buffer[..count].to_vec())).is_err() {
                        break;
                    }
                }
                Err(error) => {
                    let _ = tx.send(Err(error.to_string()));
                    break;
                }
            }
        }
    });
    rx
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn telemetry_is_opt_in_validated_and_separate_from_terminal_mode() {
        let plain = Cli::try_parse_from(["rbf", "program.bf"]).unwrap();
        assert!(!plain.telemetry);
        assert!(plain.telemetry_interval.is_none());
        let enabled = Cli::try_parse_from([
            "rbf",
            "program.bf",
            "--telemetry",
            "--telemetry-interval",
            "0.25",
        ])
        .unwrap();
        assert_eq!(enabled.telemetry_interval, Some(Duration::from_millis(250)));
        assert!(Cli::try_parse_from(["rbf", "program.bf", "--telemetry-interval", "1"]).is_err());
        assert!(Cli::try_parse_from(["rbf", "program.bf", "--telemetry", "--terminal"]).is_err());
        for interval in ["0", "-1", "NaN", "inf", "3601"] {
            assert!(Cli::try_parse_from([
                "rbf",
                "program.bf",
                "--telemetry",
                "--telemetry-interval",
                interval
            ])
            .is_err());
        }
    }

    #[test]
    fn text_capture_never_takes_over_terminal_or_live_stdin() {
        let cli = Cli::try_parse_from(["rbf", "program.bf", "--terminal"]).unwrap();
        assert!(!deterministic_input(&cli));
        let cli = Cli::try_parse_from([
            "rbf",
            "program.bf",
            "--terminal",
            "--text-frame",
            "screen.text40",
        ])
        .unwrap();
        assert!(deterministic_input(&cli));
        for incompatible in ["--tty-stdin", "--control-stdin", "--headless", "--visual"] {
            assert!(
                Cli::try_parse_from(["rbf", "program.bf", "--terminal", incompatible]).is_err()
            );
        }
        for capture in ["--raw-frame", "--screenshot", "--expect-frame"] {
            assert!(Cli::try_parse_from([
                "rbf",
                "program.bf",
                "--text-frame",
                "screen.text40",
                capture,
                "pixels"
            ])
            .is_err());
        }
    }
}
