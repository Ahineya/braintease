//! Runtime services for native RCC/RFL objects. This owns memory and devices;
//! it has no instruction decoder, VM registers or interpreter dispatch loop.
mod address_space;
mod devices;
mod exports;
mod files;
mod host;
mod memory;
mod process;
mod strings;
pub use host::rcc_native_start;
use rcc_ir::target::DataLayout;
use rvm::{devices::DeviceState, display_rgb565::RGB565Display};
use std::collections::BTreeMap;
use std::io::Write;

const BANK: usize = DataLayout::RIPPLE16.bank_cells as usize;
const CELLS: usize = BANK * 256;

pub struct Runtime {
    memory: address_space::Memory,
    stack: usize,
    free: BTreeMap<usize, usize>,
    allocated: BTreeMap<usize, usize>,
    functions: BTreeMap<u32, (usize, u32)>,
    processes: process::Machine,
    output: Vec<u8>,
    devices: DeviceState,
    display: RGB565Display,
    mode: u16,
    resolution: u16,
    enabled: bool,
    flushed: bool,
    rng: u32,
    host: host::Host,
    files: BTreeMap<u64, files::Stream>,
    errno: u64,
    clock_start: Option<std::time::Instant>,
    presentations: u64,
    frame_target: u64,
    deadline: Option<std::time::Instant>,
    poll_count: u16,
}
impl Runtime {
    fn new() -> Self {
        let heap = 5 * BANK + 1;
        Self {
            memory: address_space::Memory::new(),
            stack: 2 * BANK + 1,
            free: BTreeMap::from([(heap, CELLS - heap)]),
            allocated: BTreeMap::new(),
            functions: BTreeMap::new(),
            processes: process::Machine::default(),
            output: Vec::new(),
            devices: DeviceState::default(),
            display: RGB565Display::new(),
            mode: 0,
            resolution: 0x1010,
            enabled: false,
            flushed: false,
            rng: 1,
            host: host::Host::from_env(),
            files: BTreeMap::new(),
            errno: 0,
            clock_start: None,
            presentations: 0,
            frame_target: std::env::var("RCC_NATIVE_FRAME_NUMBER")
                .ok()
                .map(|v| {
                    v.parse::<u64>()
                        .ok()
                        .filter(|n| *n > 0)
                        .unwrap_or_else(|| fatal("RCC_NATIVE_FRAME_NUMBER must be positive"))
                })
                .unwrap_or(1),
            deadline: std::env::var("RCC_NATIVE_TIMEOUT").ok().map(|v| {
                let seconds = v
                    .parse::<u64>()
                    .ok()
                    .filter(|n| *n > 0)
                    .unwrap_or_else(|| fatal("RCC_NATIVE_TIMEOUT must be positive"));
                std::time::Instant::now()
                    .checked_add(std::time::Duration::from_secs(seconds))
                    .unwrap_or_else(|| fatal("native timeout overflow"))
            }),
            poll_count: 0,
        }
    }
    fn flush(&mut self) {
        if let Err(error) = std::io::stdout()
            .write_all(&self.output)
            .and_then(|_| std::io::stdout().flush())
        {
            fatal(&format!("native stdout: {error}"));
        }
        self.output.clear();
    }
    fn putc(&mut self, value: u64) -> u64 {
        self.output.push(value as u8);
        if self.output.len() >= 8192 || value as u8 == b'\n' {
            self.flush();
        }
        value & 65535
    }
    fn poll(&mut self) {
        self.poll_count = self.poll_count.wrapping_add(1);
        if self.poll_count & 1023 != 0 {
            return;
        }
        if self
            .deadline
            .is_some_and(|deadline| std::time::Instant::now() >= deadline)
        {
            self.flush();
            fatal("native wall-time limit exceeded");
        }
        if self.host.visual() && self.display.is_window_closed() {
            self.flush();
            std::process::exit(0);
        }
    }
}
fn fatal(message: &str) -> ! {
    eprintln!("native runtime error: {message}");
    std::process::exit(1)
}
