//! Small worker-owned C ABI. No native window, host filesystem or JIT imports.
//! JS owns upload buffers and never reenters the VM during a host import.
use crate::{VM, display_rgb565::rgb565_to_rgb888, input_events::InputTransition, vm::VMState};
use std::cell::{Cell, RefCell};
#[link(wasm_import_module = "rvm_host")]
extern "C" {
    fn monotonic_ms() -> f64;
    fn realtime_ms() -> f64;
    fn random_fill(pointer: *mut u8, length: usize) -> i32;
}
thread_local! {
 static NOW:Cell<f64>=const{Cell::new(0.0)};
 static EPOCH:Cell<f64>=const{Cell::new(0.0)};
 static IDLE:Cell<Option<u16>>=const{Cell::new(None)};
 static MACHINE:RefCell<Option<Browser>>=const{RefCell::new(None)};
 static ERROR:RefCell<String>=const{RefCell::new(String::new())};
}
struct Browser {
    vm: VM,
    console: Vec<u8>,
    rgba: Vec<u8>,
    sequence: u64,
    width: u32,
    height: u32,
}
pub(crate) fn now_ms() -> f64 {
    NOW.with(Cell::get)
}
pub(crate) fn epoch_ms() -> f64 {
    EPOCH.with(Cell::get)
}
pub(crate) fn idle(ms: u16) {
    IDLE.with(|x| x.set(Some(ms)));
}
pub(crate) fn random(out: &mut [u8]) -> Result<(), ()> {
    if unsafe { random_fill(out.as_mut_ptr(), out.len()) } == 0 {
        Ok(())
    } else {
        Err(())
    }
}
fn sample() {
    NOW.with(|x| x.set(unsafe { monotonic_ms() }));
    EPOCH.with(|x| x.set(unsafe { realtime_ms() }));
}
fn failed(error: String) -> i32 {
    ERROR.with(|e| *e.borrow_mut() = error);
    -1
}
#[no_mangle]
pub extern "C" fn rvm_alloc(length: usize) -> *mut u8 {
    if length > 256 * 1024 * 1024 {
        return std::ptr::null_mut();
    }
    let mut bytes = Vec::<u8>::new();
    if bytes.try_reserve_exact(length).is_err() {
        return std::ptr::null_mut();
    };
    bytes.resize(length, 0);
    Box::into_raw(bytes.into_boxed_slice()) as *mut u8
}
#[no_mangle]
pub unsafe extern "C" fn rvm_dealloc(pointer: *mut u8, length: usize) {
    if !pointer.is_null() {
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
            pointer, length,
        )));
    }
}
#[no_mangle]
pub unsafe extern "C" fn rvm_boot(
    bios: *const u8,
    bios_length: usize,
    disk: *const u8,
    disk_length: usize,
) -> i32 {
    if bios.is_null()
        || disk.is_null()
        || bios_length > 16 * 1024 * 1024
        || disk_length > 256 * 1024 * 1024
    {
        return failed("invalid browser image sizes".into());
    }
    // Drop the old VM before allocating a replacement; resets cannot double RAM.
    MACHINE.with(|m| *m.borrow_mut() = None);
    ERROR.with(|e| e.borrow_mut().clear());
    sample();
    IDLE.with(|x| x.set(None));
    let mut vm = VM::with_memory_size(64000, 16_384_000);
    vm.host_input_enabled = false;
    if let Err(e) = vm.browser_disk(std::slice::from_raw_parts(disk, disk_length).to_vec()) {
        return failed(e);
    }
    if let Err(e) = vm.load_binary(std::slice::from_raw_parts(bios, bios_length)) {
        return failed(e);
    }
    MACHINE.with(|m| {
        *m.borrow_mut() = Some(Browser {
            vm,
            console: Vec::new(),
            rgba: Vec::new(),
            sequence: u64::MAX,
            width: 0,
            height: 0,
        })
    });
    0
}
#[no_mangle]
pub extern "C" fn rvm_advance(instructions: u32, milliseconds: f64) -> i32 {
    sample();
    let start = now_ms();
    let duration = milliseconds.clamp(1.0, 12.0);
    IDLE.with(|x| x.set(None));
    MACHINE.with(|cell| {
        let mut machine = cell.borrow_mut();
        let Some(b) = machine.as_mut() else {
            return failed("VM has not booted".into());
        };
        let mut remaining = instructions.clamp(1, 2_000_000) as u64;
        let mut since_sample = 0;
        while remaining > 0 && matches!(b.vm.state, VMState::Running) {
            if b.vm.devices.has_pending_tick() {
                if let Err(e) = b.vm.complete_tick(Vec::new()) {
                    return failed(e);
                }
            }
            let n = match b.vm.run_batch(remaining.min(8192)) {
                Ok(n) => n,
                Err(e) => return failed(e),
            };
            if let Some(e) = b.vm.device_error.take() {
                return failed(e);
            }
            remaining = remaining.saturating_sub(n);
            since_sample += n;
            // A zero-duration host yield is a checkpoint, not a browser timer.
            // Continue within this bounded turn; positive waits return to JS.
            if IDLE.with(|x| x.get().is_some_and(|ms| ms > 0)) || n == 0 {
                break;
            }
            IDLE.with(|x| x.set(None));
            if since_sample >= 8192 {
                sample();
                since_sample = 0;
                if now_ms() - start >= duration {
                    break;
                }
            }
        }
        b.console = b.vm.get_output();
        if let Some(display) = b.vm.rgb565_display.as_ref() {
            let shared = display.get_state();
            let state = shared.lock().unwrap();
            if !state.initialized {
                b.width = 0;
                b.height = 0;
                b.rgba.clear();
            } else if b.sequence != state.sequence
                || b.width != u32::from(state.width)
                || b.height != u32::from(state.height)
            {
                b.sequence = state.sequence;
                b.width = u32::from(state.width);
                b.height = u32::from(state.height);
                b.rgba.resize(state.front_buffer.len() * 4, 255);
                for (out, &pixel) in b.rgba.chunks_exact_mut(4).zip(state.front_buffer.iter()) {
                    out[..3].copy_from_slice(&rgb565_to_rgb888(pixel));
                }
            }
        }
        if matches!(b.vm.state, VMState::Halted) {
            2
        } else if IDLE.with(|x| x.get().is_some()) {
            1
        } else {
            0
        }
    })
}
#[no_mangle]
pub unsafe extern "C" fn rvm_tty(pointer: *const u8, length: usize) -> i32 {
    if length > 16384 || pointer.is_null() {
        return failed("console input batch too large".into());
    }
    MACHINE.with(|m| {
        let mut m = m.borrow_mut();
        let Some(b) = m.as_mut() else { return -1 };
        if b.vm.input_buffer.len() + length > 65536 {
            return failed("console input queue full".into());
        }
        b.vm.input_buffer
            .extend(std::slice::from_raw_parts(pointer, length));
        0
    })
}
#[no_mangle]
pub unsafe extern "C" fn rvm_events(pointer: *const u8, length: usize) -> i32 {
    if length > 32768 || pointer.is_null() {
        return failed("input event batch too large".into());
    }
    let events: Vec<InputTransition> =
        match serde_json::from_slice(std::slice::from_raw_parts(pointer, length)) {
            Ok(e) => e,
            Err(e) => return failed(e.to_string()),
        };
    if events.len() > 64 || events.iter().any(|e| !e.valid()) {
        return failed("invalid input events".into());
    }
    MACHINE.with(|m| {
        let mut m = m.borrow_mut();
        if let Some(b) = m.as_mut() {
            b.vm.devices.accept_events(events);
            0
        } else {
            -1
        }
    })
}
macro_rules! getter {
    ($name:ident,$body:expr) => {
        #[no_mangle]
        pub extern "C" fn $name() -> usize {
            MACHINE.with(|m| m.borrow().as_ref().map_or(0, $body))
        }
    };
}
getter!(
    rvm_disk_pointer,
    |b: &Browser| b.vm.browser_disk_bytes().as_ptr() as usize
);
getter!(rvm_disk_length, |b: &Browser| b
    .vm
    .browser_disk_bytes()
    .len());
getter!(rvm_console_pointer, |b: &Browser| b.console.as_ptr()
    as usize);
getter!(rvm_console_length, |b: &Browser| b.console.len());
getter!(rvm_frame_pointer, |b: &Browser| b.rgba.as_ptr() as usize);
getter!(rvm_frame_width, |b: &Browser| b.width as usize);
getter!(rvm_frame_height, |b: &Browser| b.height as usize);
getter!(rvm_frame_sequence, |b: &Browser| b.sequence as usize);
#[no_mangle]
pub extern "C" fn rvm_idle_ms() -> u32 {
    IDLE.with(|x| u32::from(x.get().unwrap_or(0)))
}
#[no_mangle]
pub extern "C" fn rvm_error_pointer() -> usize {
    ERROR.with(|e| e.borrow().as_ptr() as usize)
}
#[no_mangle]
pub extern "C" fn rvm_error_length() -> usize {
    ERROR.with(|e| e.borrow().len())
}
