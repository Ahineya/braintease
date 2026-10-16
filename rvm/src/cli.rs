use clap::Parser;
use rvm::runner::ClockPolicy;
use std::path::PathBuf;

#[derive(Parser, Debug)]
#[command(
    name = "rvm",
    about = "Ripple Virtual Machine - Run Ripple VM binary programs",
    long_about = "The Ripple Virtual Machine (RVM) executes binary programs compiled for the Ripple architecture.\n\
                  It features memory-mapped I/O, multiple display modes, storage, and debugging capabilities.",
    version,
    author
)]
pub struct Cli {
    /// Binary file to execute
    pub binary_file: PathBuf,

    /// Set bank size
    #[arg(short = 'b', long, default_value = "65535")]
    pub bank_size: u16,

    /// Set memory size in words
    #[arg(short = 'm', long)]
    pub memory: Option<usize>,

    /// Set virtual CPU frequency (e.g., 1MHz, 500KHz, 2.5GHz)
    #[arg(short = 'f', long)]
    pub frequency: Option<String>,

    /// Set RNG seed (hex with 0x prefix or decimal)
    #[arg(short = 's', long)]
    pub seed: Option<String>,

    /// Pre-populate input buffer with text
    #[arg(short = 'i', long)]
    pub input: Option<String>,

    /// Enable debug mode (step through execution)
    #[arg(short = 'd', long)]
    pub debug: bool,

    /// Enable TUI debugger mode
    #[arg(short = 't', long)]
    pub tui: bool,

    /// Show VM state during execution
    #[arg(short = 'v', long)]
    pub verbose: bool,

    /// Enable RGB565 visual display window
    #[arg(long)]
    pub visual: bool,

    /// Run without a window or live terminal/keyboard input
    #[arg(long, conflicts_with_all = ["visual", "debug", "tui"])]
    pub headless: bool,
    /// Send Ctrl-C to the guest TTY; Ctrl-] remains an emergency host stop.
    #[arg(long)]
    pub guest_ctrl_c: bool,
    /// Save a native-resolution PNG and stop at the requested presentation
    #[arg(long, conflicts_with_all = ["debug", "tui"])]
    pub screenshot: Option<PathBuf>,
    /// Also save canonical little-endian RGB565 pixels (no header)
    #[arg(long, conflicts_with_all = ["debug", "tui"])]
    pub raw_frame: Option<PathBuf>,
    /// Presentation number to capture (one-based)
    #[arg(long, default_value_t = 1)]
    pub capture_frame: u64,
    /// Compare captured pixels to a raw RGB565 baseline; write diff artifacts on failure
    #[arg(long, requires = "screenshot")]
    pub expect_frame: Option<PathBuf>,
    /// Write a JSON execution report, including input artifact hashes
    #[arg(long, conflicts_with_all = ["debug", "tui"])]
    pub report: Option<PathBuf>,
    /// Fail after this many guest instructions unless already finished
    #[arg(long, conflicts_with_all = ["debug", "tui"])]
    pub max_instructions: Option<u64>,
    /// Stop successfully before granting a tick beyond this limit
    #[arg(long, conflicts_with_all = ["debug", "tui"])]
    pub max_ticks: Option<u64>,
    /// Host wall-time safeguard in seconds (also applies while waiting for a step)
    #[arg(long, conflicts_with_all = ["debug", "tui"])]
    pub timeout: Option<u64>,
    /// Logical clock scheduling, independent of instruction frequency
    #[arg(long, value_enum, default_value = "unthrottled", conflicts_with_all = ["debug", "tui"])]
    pub clock: ClockPolicy,
    /// Logical ticks per second (Doom uses 35)
    #[arg(long, default_value_t = 35, value_parser = clap::value_parser!(u16).range(1..))]
    pub tick_hz: u16,
    /// JSON array of typed keyboard/pointer transitions scheduled by logical tick
    #[arg(long, conflicts_with_all = ["debug", "tui"])]
    pub input_script: Option<PathBuf>,
    /// Read JSON-line step/key/input/quit commands from stdin; required for stepped clock
    #[arg(long, conflicts_with_all = ["debug", "tui", "input"])]
    pub control_stdin: bool,

    /// Path to disk image file for storage (default: ~/.RippleVM/disk.img)
    #[arg(long)]
    pub disk: Option<PathBuf>,

    /// NIC policy: auto (default), host, shared, off, or an external helper socket
    #[arg(long)]
    pub net: Option<PathBuf>,

    /// After HALT, exit with main()'s return value in RV0 (low 8 bits).
    /// Used by the c-testsuite runner so `return 1` fails the test.
    #[arg(long)]
    pub exit_code: bool,

    /// Compile executed basic blocks to native code with Cranelift.
    /// Debug, TUI, and --verbose stay on the interpreter.
    #[arg(long)]
    pub jit: bool,
}

impl Cli {
    /// Parse RNG seed from string (hex or decimal)
    pub fn parse_seed(&self) -> Option<u32> {
        self.seed.as_ref().map(|s| {
            if s.starts_with("0x") || s.starts_with("0X") {
                u32::from_str_radix(&s[2..], 16).unwrap_or_else(|_| {
                    eprintln!("Error: Invalid hex seed: {}", s);
                    std::process::exit(1);
                })
            } else {
                s.parse().unwrap_or_else(|_| {
                    eprintln!("Error: Invalid seed: {}", s);
                    std::process::exit(1);
                })
            }
        })
    }

    /// Parse frequency from string
    pub fn parse_frequency(&self) -> Option<u64> {
        self.frequency.as_ref().map(|s| {
            parse_frequency(s).unwrap_or_else(|e| {
                eprintln!("Error: {}", e);
                std::process::exit(1);
            })
        })
    }
}

/// Parse frequency from string (e.g., "1MHz", "500KHz", "1000000", "2.5MHz")
pub fn parse_frequency(s: &str) -> Result<u64, String> {
    let s = s.trim();

    // Check for suffix
    if let Some(num_str) = s.strip_suffix("GHz") {
        parse_float_with_multiplier(num_str, 1_000_000_000)
    } else if let Some(num_str) = s.strip_suffix("MHz") {
        parse_float_with_multiplier(num_str, 1_000_000)
    } else if let Some(num_str) = s.strip_suffix("KHz") {
        parse_float_with_multiplier(num_str, 1_000)
    } else if let Some(num_str) = s.strip_suffix("kHz") {
        parse_float_with_multiplier(num_str, 1_000)
    } else if let Some(num_str) = s.strip_suffix("Hz") {
        parse_float_with_multiplier(num_str, 1)
    } else {
        // Try to parse as plain number (assumed to be Hz)
        s.parse::<u64>()
            .map_err(|_| format!("Invalid frequency value: {s}"))
    }
}

fn parse_float_with_multiplier(s: &str, multiplier: u64) -> Result<u64, String> {
    let s = s.trim();
    if let Ok(f) = s.parse::<f64>() {
        Ok((f * multiplier as f64) as u64)
    } else {
        Err(format!("Invalid numeric value: {s}"))
    }
}
