#![cfg_attr(target_os = "fuzix", no_std)]
#![cfg_attr(target_os = "fuzix", no_main)]
extern crate alloc;
mod probes;

#[cfg(target_os = "fuzix")]
unsafe extern "C" {
    fn rustcell_panic() -> !;
}
#[cfg(target_os = "fuzix")]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { rustcell_panic() }
}

#[cfg(target_os = "fuzix")]
pub fn rust_entry() -> u16 {
    let arguments = match rustcell::env::args() {
        Ok(arguments) => arguments,
        Err(_) => return 249,
    };
    if let Some(mode) = arguments.get(1) {
        #[cfg(feature = "dynamic")]
        if mode.as_slice() == b"callback".as_slice() {
            return probes::blocking();
        }
        if mode.as_slice() == b"panic".as_slice() {
            // Exercise both arms of Result::unwrap with a runtime condition.
            let result: Result<u16, u8> = if mode.len() == 5 { Err(7) } else { Ok(123) };
            return result.unwrap();
        }
        if mode.as_slice() == b"formatted".as_slice() {
            fn argument() -> u16 {
                let _ = rustcell::io::write_stdout(b"PANIC_ARGUMENT\n");
                7
            }
            panic!("argument {}", argument());
        }
        #[cfg(feature = "byteorder")]
        if mode.as_slice() == b"short".as_slice() {
            use byteorder::ByteOrder;
            return byteorder::BigEndian::read_u32(mode.as_slice().get(..1).unwrap()) as u16;
        }
    }
    match probes::run() {
        Ok(bytes) => {
            if rustcell::fs::write(b"/crate.out", &bytes).is_err() {
                return 250;
            }
            if rustcell::io::write_stdout(b"CRATES_OK\n").is_err() {
                return 251;
            }
            0
        }
        Err(code) => code,
    }
}

#[cfg(not(target_os = "fuzix"))]
fn main() {
    use std::io::Write;
    let bytes = probes::run().expect("crate compatibility checks");
    std::io::stdout().write_all(&bytes).unwrap();
}
