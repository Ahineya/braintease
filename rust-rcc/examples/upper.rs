//! Run as: upper INPUT OUTPUT. Only ASCII letters are changed; all other bytes
//! (including NUL and non-UTF-8 data) are copied exactly.
#![no_std]
#![no_main]
mod utility;
unsafe extern "C" {
    fn rustcell_panic() -> !;
}
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { rustcell_panic() }
}
pub fn rust_entry() -> u16 {
    let arguments = match rustcell::env::args() {
        Ok(args) => args,
        Err(_) => return 1,
    };
    if arguments.len() != 3 {
        let _ = rustcell::io::write_stderr(b"usage: upper INPUT OUTPUT\n");
        return 2;
    }
    match utility::uppercase(&arguments[1], &arguments[2]) {
        Ok(()) => 0,
        Err(_) => {
            let _ = rustcell::io::write_stderr(b"upper: file operation failed\n");
            1
        }
    }
}
