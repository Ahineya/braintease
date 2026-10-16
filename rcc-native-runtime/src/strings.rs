//! Cell-oriented C strings for the compatibility data layout.
use super::memory::{linear, pointer};
use super::*;
fn range(ptr: u64, count: u64) -> std::ops::Range<usize> {
    let at = linear(ptr);
    let end = at
        .checked_add(
            count
                .try_into()
                .unwrap_or_else(|_| fatal("string length overflow")),
        )
        .filter(|e| *e <= CELLS)
        .unwrap_or_else(|| fatal("string span out of bounds"));
    at..end
}
fn length(rt: &Runtime, ptr: u64) -> usize {
    let at = linear(ptr);
    rt.memory
        .cells(at..CELLS)
        .position(|c| c == 0)
        .unwrap_or_else(|| fatal("unterminated C string"))
}
macro_rules! string_service {
    ($name:ident($rt:ident $(,$arg:ident)*) $body:block) => {
        #[no_mangle]
        unsafe extern "C" fn $name(ctx: *mut Runtime, $($arg: u64),*) -> u64 {
            let $rt = unsafe { &mut *ctx };
            $body
        }
    };
}
fn compare(rt: &Runtime, a: u64, b: u64, count: u64, strings: bool) -> u64 {
    let (mut a, mut b) = (linear(a), linear(b));
    for _ in 0..count {
        if a >= CELLS || b >= CELLS {
            fatal("C comparison out of bounds");
        }
        let (x, y) = (rt.memory.read(a), rt.memory.read(b));
        if x != y {
            return if x < y { u64::MAX } else { 1 };
        }
        if strings && x == 0 {
            break;
        }
        a += 1;
        b += 1;
    }
    0
}
string_service!(rcc_native_ext_strcmp(rt, a, b) {
    compare(rt, a, b, CELLS as u64, true)
});
string_service!(rcc_native_ext_strncmp(rt, a, b, count) {
    compare(rt, a, b, count, true)
});
string_service!(rcc_native_ext_memcmp(rt, a, b, count) {
    range(a, count);
    range(b, count);
    compare(rt, a, b, count, false)
});
string_service!(rcc_native_ext_strcpy(rt, dest, src) {
    let count = length(rt, src) + 1;
    let from = range(src, count as u64);
    let to = range(dest, count as u64);
    rt.memory.copy_within(from, to.start);
    dest
});
string_service!(rcc_native_ext_strncpy(rt, dest, src, count) {
    let to = range(dest, count);
    let mut from = linear(src);
    let mut ended = false;
    for at in to {
        if !ended {
            if from >= CELLS { fatal("strncpy source out of bounds"); }
            ended = rt.memory.read(from) == 0;
        }
        rt.memory.write(at, if ended { 0 } else { rt.memory.read(from) });
        if !ended { from += 1; }
    }
    dest
});
string_service!(rcc_native_ext_strcat(rt, dest, src) {
    let old = length(rt, dest);
    let count = length(rt, src) + 1;
    let from = range(src, count as u64);
    let to = range(pointer(linear(dest) + old), count as u64);
    rt.memory.copy_within(from, to.start);
    dest
});
string_service!(rcc_native_ext_strchr(rt, src, value) {
    let at = linear(src);
    let n = length(rt, src);
    rt.memory.cells(at..at + n + 1).position(|v| v == value as u16)
        .map(|i| pointer(at + i)).unwrap_or(0)
});
string_service!(rcc_native_ext_strrchr(rt, src, value) {
    let at = linear(src);
    let n = length(rt, src);
    rt.memory.cells(at..at + n + 1).rposition(|v| v == value as u16)
        .map(|i| pointer(at + i)).unwrap_or(0)
});
string_service!(rcc_native_ext_memchr(rt, src, value, count) {
    let r = range(src, count);
    let start = r.start;
    rt.memory.cells(r).position(|v| v == value as u16)
        .map(|i| pointer(start + i)).unwrap_or(0)
});
