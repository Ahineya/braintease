#![no_std]
#![no_main]
#[path = "../examples/heap.rs"]
mod heap;
unsafe extern "C" {
    fn rustcell_panic() -> !;
}
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { rustcell_panic() }
}
pub fn rust_entry() -> u16 {
    heap::check()
}
