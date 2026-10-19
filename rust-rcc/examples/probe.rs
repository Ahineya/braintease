#![no_std]
#![no_main]

unsafe extern "C" {
    fn rustcell_report(value: u16);
    fn rustcell_panic() -> !;
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { rustcell_panic() }
}

#[unsafe(no_mangle)]
pub fn rust_entry() -> u16 {
    let mut words = [0x1234u16, 0xabcd];
    let bytes = words.as_mut_ptr().cast::<u8>();
    unsafe {
        rustcell_report(core::mem::size_of::<u16>() as u16);
        rustcell_report(*bytes as u16);
        *bytes.add(1) = 0x56;
        rustcell_report(words[0]);
    }
    0
}
