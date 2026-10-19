#![no_std]
#![no_main]
include!(concat!(env!("OUT_DIR"), "/root.rs"));
unsafe extern "C" {
    fn rustcell_panic() -> !;
}
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { rustcell_panic() }
}

pub fn rust_entry() -> u16 {
    if cell_math::checksum(&[0x80, 0xff, 0x12]) != 408 {
        return 1;
    }
    if cell_math::apply(0x1234_0000u32, 0x5678) != 0x1234_5678 {
        return 2;
    }
    if cell_macros::number!() != ROOT_NUMBER
        || env!("CELL_GENERATED").len() != 14
        || env!("CARGO_PKG_NAME").len() != 12
    {
        return 4;
    }
    match rustcell::io::write_stdout(cell_math::message().as_bytes()) {
        Ok(()) => 0,
        Err(_) => 3,
    }
}
