#![no_std]
#![no_main]

#[path = "../examples/values.rs"]
mod values;

unsafe extern "C" {
    fn rustcell_panic() -> !;
}
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { rustcell_panic() }
}

pub fn rust_entry() -> u16 {
    let enums = values::enums();
    if enums != 0 {
        return enums;
    }
    let drops = values::drops();
    if drops != 0 {
        return 30 + drops;
    }
    0
}
