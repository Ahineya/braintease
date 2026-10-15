//! Runtime services for native RCC/RFL objects. This owns memory and devices;
//! it has no instruction decoder, VM registers or interpreter dispatch loop.
mod devices;
mod exports;
mod memory;
use rcc_ir::target::DataLayout;
use rvm::{devices::DeviceState, display_rgb565::RGB565Display};
use std::collections::BTreeMap;
use std::io::Write;

const BANK: usize = DataLayout::RIPPLE16.bank_cells as usize;
const CELLS: usize = BANK * 256;

pub struct Runtime {
    memory: Vec<u16>,
    stack: usize,
    free: BTreeMap<usize, usize>,
    allocated: BTreeMap<usize, usize>,
    functions: BTreeMap<u32, (usize, u32)>,
    output: Vec<u8>,
    devices: DeviceState,
    display: RGB565Display,
    mode: u16,
    resolution: u16,
    enabled: bool,
    flushed: bool,
    rng: u32,
    visual: bool,
}
impl Runtime {
    fn new() -> Self {
        let heap = 5 * BANK + 1;
        Self {
            memory: vec![0; CELLS],
            stack: 2 * BANK + 1,
            free: BTreeMap::from([(heap, CELLS - heap)]),
            allocated: BTreeMap::new(),
            functions: BTreeMap::new(),
            output: Vec::new(),
            devices: DeviceState::default(),
            display: RGB565Display::new(),
            mode: 0,
            resolution: 0x1010,
            enabled: false,
            flushed: false,
            rng: 1,
            visual: std::env::var_os("RCC_NATIVE_VISUAL").is_some(),
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
        if self.visual && self.display.is_window_closed() {
            self.flush();
            std::process::exit(0);
        }
    }
}
fn fatal(message: &str) -> ! {
    eprintln!("native runtime error: {message}");
    std::process::exit(1)
}

/// # Safety
/// `entry` must be the synchronous native entry emitted by rcc-native, with
/// signature `extern "C" fn(*mut Runtime) -> u64`, valid for this process.
#[no_mangle]
pub unsafe extern "C" fn rcc_native_start(entry: usize) -> i32 {
    let entry: unsafe extern "C" fn(*mut Runtime) -> u64 = unsafe { std::mem::transmute(entry) };
    let mut runtime = Runtime::new();
    if let Some(path) = std::env::var_os("RCC_NATIVE_INPUT_SCRIPT") {
        let script = rvm::devices::InputScript::from_path(path).unwrap_or_else(|e| fatal(&e));
        runtime
            .devices
            .set_script(script)
            .unwrap_or_else(|e| fatal(&e));
    }
    if runtime.visual {
        let state = runtime.display.get_state();
        let worker = std::thread::spawn(move || {
            let status = unsafe { entry(&mut runtime) };
            runtime.flush();
            runtime.display.shutdown();
            status
        });
        if let Err(error) = rvm::display_rgb565::run_rgb565_display(state) {
            fatal(&format!("native display: {error}"));
        }
        worker
            .join()
            .unwrap_or_else(|_| fatal("native worker panicked")) as i32
    } else {
        let status = unsafe { entry(&mut runtime) };
        runtime.flush();
        if std::env::var_os("RCC_NATIVE_FRAME").is_some() {
            fatal("requested native frame was never presented");
        }
        status as i32
    }
}
