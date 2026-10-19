#![no_std]
#![no_main]
#![allow(unnecessary_transmutes)] // Deliberately test byte-wise representation.

mod files;
mod heap;
mod utility;
mod values;

unsafe extern "C" {
    fn rustcell_report(value: u16);
    fn rustcell_write(bytes: *const u8, length: usize) -> isize;
    fn rustcell_fork() -> i16;
    fn rustcell_wait(pid: i16) -> i16;
    fn rustcell_sleep(seconds: u16) -> u16;
    fn rustcell_panic() -> !;
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { rustcell_panic() }
}

fn print(text: &str) {
    unsafe {
        rustcell_write(text.as_ptr(), text.len());
    }
}

#[inline(never)]
fn identity<T: Copy>(value: T) -> T {
    value
}

trait Transform {
    fn transform(self) -> Self;
}
impl Transform for u16 {
    #[inline(never)]
    fn transform(self) -> Self {
        self ^ 0x55aa
    }
}

#[repr(C)]
#[derive(Copy, Clone)]
struct Record {
    prefix: u8,
    word: u16,
    number: u32,
}

#[inline(never)]
fn change(mut record: Record) -> Record {
    record.word = record.word ^ 0x0100;
    record
}

#[inline(never)]
fn first(words: &[u16; 2]) -> u16 {
    words[0]
}

#[inline(never)]
fn mutate(words: &mut [u16; 2]) {
    words[0] = 0x5678;
}

#[inline(never)]
fn layout_check(seed: u16) -> u16 {
    unsafe {
        rustcell_report(1);
    }
    if core::mem::size_of::<u8>() != 1
        || core::mem::size_of::<u16>() != 2
        || core::mem::size_of::<u32>() != 4
        || core::mem::size_of::<u64>() != 8
        || core::mem::size_of::<usize>() != 4
    {
        return 1;
    }
    let mut words = [seed, 0xabcdu16];
    let bytes = words.as_mut_ptr().cast::<u8>();
    unsafe {
        if *bytes != 0x34 || *bytes.add(1) != 0x12 || *bytes.add(2) != 0xcd {
            return 2;
        }
        *bytes.add(1) = 0x56;
    }
    if words[0] != 0x5634 || words[1] != 0xabcd {
        return 3;
    }
    if words[(seed & 1) as usize] != 0x5634 {
        return 10;
    }
    if first(&words) != 0x5634 {
        return 12;
    }
    mutate(&mut words);
    if first(&words) != 0x5678 {
        return 13;
    }
    words[0] = 0x5634;
    unsafe {
        rustcell_report(2);
    }
    let raw: [u8; 2] = unsafe { core::mem::transmute(words[0]) };
    if raw[0] != 0x34 || raw[1] != 0x56 {
        return 4;
    }
    let decoded: u16 = unsafe { core::mem::transmute(raw) };
    if decoded != words[0] {
        return 5;
    }
    unsafe {
        rustcell_report(3);
    }
    if identity(0x12345678u32) != 0x12345678 || identity(0x1234u16).transform() != 0x479e {
        return 6;
    }
    unsafe {
        rustcell_report(4);
    }
    let signed = identity(-2i8);
    if signed as i32 != -2 || signed as u8 != 254 || signed >> 1 != -1 {
        return 7;
    }
    if identity(255u8) + identity(1u8) != 0 || identity(1u8) << identity(8u8) != 1 {
        return 11;
    }
    unsafe {
        rustcell_report(5);
    }
    let wide = identity(0x123456789abcdef0u64);
    if (wide >> 32) as u32 != 0x12345678 || wide as u32 != 0x9abcdef0 {
        return 8;
    }
    unsafe {
        rustcell_report(6);
    }
    let original = Record {
        prefix: 0x80,
        word: seed,
        number: 0xfedcba98,
    };
    let changed = change(original);
    if original.word != seed || changed.word != seed ^ 0x0100 || changed.number != 0xfedcba98 {
        return 9;
    }
    unsafe {
        rustcell_report(7);
    }
    0
}

#[unsafe(no_mangle)]
pub fn rust_entry() -> u16 {
    unsafe {
        rustcell_report(0);
    }
    let status = layout_check(0x1234);
    if status != 0 {
        unsafe {
            rustcell_report(100 + status);
        }
        return status;
    }
    print("RUSTCELL_LAYOUT_OK\n");
    let status = values::enums();
    if status != 0 {
        unsafe {
            rustcell_report(200 + status);
        }
        return 30 + status;
    }
    print("RUSTCELL_ENUMS_OK\n");
    let status = values::drops();
    if status != 0 {
        unsafe {
            rustcell_report(300 + status);
        }
        return 60 + status;
    }
    print("RUSTCELL_DROPS_OK\n");
    let status = heap::check();
    if status != 0 {
        unsafe {
            rustcell_report(400 + status);
        }
        return 90 + status;
    }
    print("RUSTCELL_HEAP_OK\n");
    if let Err(error) = files::check() {
        unsafe {
            rustcell_report(match error {
                rustcell::Error::Os(code) => 500 + code as u16,
                _ => 599,
            });
        }
        return 25;
    }
    // This read blocks until the integration harness supplies a byte. The local
    // and the nested Rust frames must survive the guest's syscall suspension.
    let preserved = identity(0x87654321u32);
    let mut memory = heap::fork_buffer();
    let mut input = [0u8; 1];
    print("RUSTCELL_WAITING\n");
    let count = values::with_drop(|| match rustcell::io::read_stdin(&mut input) {
        Ok(count) => count as isize,
        Err(_) => -1,
    });
    if count != 1 || input[0] != b'R' || preserved != 0x87654321 || memory[0] != 0x1234 {
        return 20;
    }
    print("RUSTCELL_IO_OK\n");
    let pid = unsafe { rustcell_fork() };
    if pid < 0 {
        return 21;
    }
    if pid == 0 {
        memory[0] = 0x5678;
        for i in 0..8u16 {
            memory.push(0x9876 + i);
        }
        unsafe {
            rustcell_sleep(1);
        }
        if memory[0] != 0x5678 || memory.len() != 10 || memory[9] != 0x987d {
            return 22;
        }
        print("RUSTCELL_CHILD_OK\n");
        return 37;
    }
    if unsafe { rustcell_wait(pid) } != 37
        || memory[0] != 0x1234
        || memory[1] != 0xabcd
        || memory.len() != 2
    {
        return 23;
    }
    for i in 0..8u16 {
        memory.push(0x5550 + i);
    }
    if memory.len() != 10 || memory[9] != 0x5557 {
        return 24;
    }
    print("RUSTCELL_FORK_OK\n");
    0
}
