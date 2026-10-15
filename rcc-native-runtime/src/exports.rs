//! Explicit adapters: compatibility pointers/strings never cross into libc.
use super::memory::linear;
use super::*;
use std::io::Read;

// Each generated call holds the runtime exclusively for its synchronous duration.
macro_rules! service {
    ($name:ident($rt:ident $(,$arg:ident)*) $body:block) => {
        #[no_mangle]
        unsafe extern "C" fn $name(ctx: *mut Runtime, $($arg: u64),*) -> u64 {
            let $rt = unsafe { &mut *ctx };
            $body
        }
    };
}
fn span(ptr: u64, count: u64) -> std::ops::Range<usize> {
    let start = linear(ptr);
    let count = usize::try_from(count).unwrap_or_else(|_| fatal("native memory length overflow"));
    let end = start
        .checked_add(count)
        .filter(|end| *end <= CELLS)
        .unwrap_or_else(|| fatal("native memory span out of bounds"));
    start..end
}
fn copy(rt: &mut Runtime, dest: u64, src: u64, count: u64) -> u64 {
    let to = span(dest, count);
    let from = span(src, count);
    if to.start >= BANK && from.start >= BANK {
        rt.memory.copy_within(from, to.start);
    } else if to.start > from.start {
        for i in (0..from.len()).rev() {
            let value = rt.read(from.start + i);
            rt.write(to.start + i, value);
        }
    } else {
        for i in 0..from.len() {
            let value = rt.read(from.start + i);
            rt.write(to.start + i, value);
        }
    }
    dest
}
service!(rcc_native_ext_putchar(rt, value) {
    rt.putc(value)
});
service!(rcc_native_ext_puts(rt, text) {
    let mut at = linear(text);
    while at < CELLS {
        let value = rt.read(at);
        if value == 0 { rt.putc(10); return 0; }
        rt.putc(u64::from(value));
        at += 1;
    }
    fatal("unterminated native string")
});
service!(rcc_native_ext_getchar(rt) {
    rt.flush();
    let mut byte = [0];
    match std::io::stdin().read(&mut byte) {
        Ok(1) => u64::from(byte[0]),
        Ok(_) => 65535,
        Err(error) => fatal(&format!("native stdin: {error}")),
    }
});
service!(rcc_native_ext_malloc(rt, size) { rt.allocate(size) });
service!(rcc_native_ext_free(rt, ptr) { rt.release(ptr); 0 });
service!(rcc_native_ext_calloc(rt, count, size) {
    let total = count.checked_mul(size).unwrap_or(0);
    let ptr = rt.allocate(total);
    if ptr != 0 { rt.memory[span(ptr, total)].fill(0); }
    ptr
});
service!(rcc_native_ext_realloc(rt, old, size) {
    if old == 0 { return rt.allocate(size); }
    if size == 0 { rt.release(old); return 0; }
    let old_at = linear(old);
    let old_size = *rt.allocated.get(&old_at).unwrap_or_else(|| fatal("invalid native realloc"));
    let next = rt.allocate(size);
    if next != 0 {
        let at = linear(next);
        let count = old_size.min(size as usize);
        rt.memory.copy_within(old_at..old_at + count, at);
        rt.release(old);
    }
    next
});
service!(rcc_native_ext_memset(rt, dest, value, count) {
    let range = span(dest, count);
    if range.start >= BANK { rt.memory[range].fill(value as u16); }
    else { for at in range { rt.write(at, value as u16); } }
    dest
});
service!(rcc_native_ext_memcpy(rt, dest, src, count) { copy(rt, dest, src, count) });
service!(rcc_native_ext_memmove(rt, dest, src, count) { copy(rt, dest, src, count) });
service!(rcc_native_ext_mmio_read(rt, address) {
    if address >= BANK as u64 { fatal("native MMIO address out of bounds"); }
    u64::from(rt.read(address as usize))
});
service!(rcc_native_ext_mmio_write(rt, address, value) {
    if address >= BANK as u64 { fatal("native MMIO address out of bounds"); }
    rt.write(address as usize, value as u16);
    0
});
service!(rcc_native_ext_rfl_halt(rt) { rt.flush(); std::process::exit(1) });
service!(rcc_native_ext_exit(rt, status) { rt.flush(); std::process::exit(status as i32) });
service!(rcc_native_ext_strlen(rt, text) {
    let at = linear(text);
    for i in at..CELLS { if rt.read(i) == 0 { return (i - at) as u64; } }
    fatal("unterminated native strlen")
});
service!(rcc_native_ext___rfl_host_sin(_rt, x) { f64::from_bits(x).sin().to_bits() });
service!(rcc_native_ext___rfl_host_cos(_rt, x) { f64::from_bits(x).cos().to_bits() });
service!(rcc_native_ext___rcc_f64_floor(_rt, x) { f64::from_bits(x).floor().to_bits() });
service!(rcc_native_ext___rcc_f64_sqrt(_rt, x) { f64::from_bits(x).sqrt().to_bits() });
