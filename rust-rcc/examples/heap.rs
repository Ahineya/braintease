//! Shared with the native Rust reference. Exercise the real alloc crate.
#![allow(dead_code)]
extern crate alloc;
use alloc::{boxed::Box, string::String, vec::Vec};

#[inline(never)]
pub fn check() -> u16 {
    let mut word = Box::new(0x1234u16);
    *word += 1;
    if *word != 0x1235 {
        return 1;
    }
    let mut words = Vec::new();
    for i in 0..40u16 {
        words.push(i * 3);
    }
    if words.len() != 40 || words[17] != 51 {
        return 2;
    }
    words.insert(3, 99);
    if words.remove(3) != 99 {
        return 3;
    }
    match words.pop() {
        Some(117) => {}
        _ => return 3,
    }
    let mut text = String::from("hello");
    text.push_str(" world");
    if text.as_bytes()[6] != b'w' || text.len() != 11 {
        return 4;
    }
    let allocation = allocation_check();
    if allocation != 0 {
        return allocation;
    }
    let drops = collection_drops();
    if drops != 0 {
        return drops;
    }
    if arithmetic_check() != 0 {
        return 30;
    }
    0
}

#[inline(never)]
fn allocation_check() -> u16 {
    use alloc::alloc::{Layout, alloc, alloc_zeroed, dealloc, realloc};
    unsafe {
        let mut align = 1;
        while align <= 32_768 {
            let layout = Layout::from_size_align_unchecked(3, align);
            let p = alloc(layout);
            if p.is_null() || (p as usize) & (align - 1) != 0 {
                return 23;
            }
            *p = 0x71;
            *p.add(2) = 0x73;
            if *p != 0x71 || *p.add(2) != 0x73 {
                return 24;
            }
            dealloc(p, layout);
            align *= 2;
        }
        // More cumulative allocation than a guest heap can hold: deallocation
        // must return usable space, rather than just running Rust destructors.
        let layout = Layout::from_size_align_unchecked(100_001, 8);
        for _ in 0..24u16 {
            let p = alloc(layout);
            if p.is_null() {
                return 25;
            }
            dealloc(p, layout);
        }
        #[cfg(target_env = "rustcell")]
        {
            // Over-aligned allocations cannot span banks in this pointer ABI.
            let layout = Layout::from_size_align_unchecked(65_536, 1024);
            if !alloc(layout).is_null() {
                return 26;
            }
        }
        let layout = Layout::from_size_align_unchecked(70_032, 256);
        let p = alloc_zeroed(layout);
        if p.is_null() {
            return 10;
        }
        if (p as usize) & 255 != 0 || *p.add(63_999) != 0 || *p.add(70_031) != 0 {
            return 11;
        }
        core::ptr::write_bytes(p, 0x5a, 70_032);
        *p.add(64_001) = 0xab;
        core::ptr::copy(p.add(1), p, 70_031);
        if *p.add(64_000) != 0xab || *p.add(70_030) != 0x5a {
            return 12;
        }
        if p.add(70_031).offset_from(p) != 70_031 || p.offset_from(p.add(70_031)) != -70_031 {
            return 13;
        }
        let next = realloc(p, layout, 80_048);
        if next.is_null() || (next as usize) & 255 != 0 {
            return 14;
        }
        if *next.add(64_000) != 0xab || *next.add(70_030) != 0x5a {
            return 15;
        }
        let layout = Layout::from_size_align_unchecked(80_048, 256);
        let smaller = realloc(next, layout, 16);
        if smaller.is_null() || *smaller.add(15) != 0x5a {
            return 16;
        }
        dealloc(smaller, Layout::from_size_align_unchecked(16, 256));
    }
    let mut bytes = Vec::<u8>::new();
    bytes.push(0x42);
    if bytes.try_reserve_exact(isize::MAX as usize).is_ok() || bytes[0] != 0x42 {
        return 17;
    }
    #[cfg(target_env = "rustcell")]
    if bytes.try_reserve_exact(1_100_000).is_ok() || bytes[0] != 0x42 {
        return 18;
    }
    // Failure must preserve the old allocation; it remains usable and growable.
    bytes.push(0x43);
    if bytes.len() != 2 || bytes[1] != 0x43 {
        return 19;
    }
    0
}

struct Counted<'a>(&'a core::cell::Cell<u16>);
impl Drop for Counted<'_> {
    fn drop(&mut self) {
        self.0.set(self.0.get() + 1);
    }
}

#[inline(never)]
fn collection_drops() -> u16 {
    let count = core::cell::Cell::new(0);
    {
        let mut owned = Vec::new();
        for _ in 0..7 {
            owned.push(Box::new(Counted(&count)));
        }
        owned.truncate(3);
        if count.get() != 4 {
            return 20;
        }
        let _slice = owned.into_boxed_slice();
    }
    if count.get() != 7 {
        return 21;
    }
    let mut zero_sized = Vec::<()>::new();
    for _ in 0..10 {
        zero_sized.push(());
    }
    if zero_sized.len() != 10 {
        return 22;
    }
    0
}

pub fn fork_buffer() -> Vec<u16> {
    let mut words = Vec::new();
    words.push(0x1234);
    words.push(0xabcd);
    words
}

#[inline(never)]
fn opaque<T>(value: T) -> T {
    value
}

fn arithmetic_check() -> u16 {
    let (value, overflow) = opaque(255u8).overflowing_add(1);
    if value != 0 || !overflow {
        return 1;
    }
    let (value, overflow) = opaque(0u8).overflowing_sub(1);
    if value != 255 || !overflow {
        return 2;
    }
    let (value, overflow) = opaque(128u8).overflowing_mul(2);
    if value != 0 || !overflow {
        return 3;
    }
    let (value, overflow) = opaque(128u8).overflowing_mul(0);
    if value != 0 || overflow {
        return 4;
    }
    let (value, overflow) = opaque(u32::MAX).overflowing_mul(2);
    if value != u32::MAX - 1 || !overflow {
        return 5;
    }
    let (value, overflow) = opaque(u64::MAX).overflowing_add(1);
    if value != 0 || !overflow {
        return 6;
    }
    let (value, overflow) = opaque(127i8).overflowing_add(1);
    if value != -128 || !overflow {
        return 7;
    }
    let (value, overflow) = opaque(i64::MIN).overflowing_sub(1);
    if value != i64::MAX || !overflow {
        return 8;
    }
    if opaque(0x8000_0000_0000_0103u64).count_ones() != 4 {
        return 9;
    }
    0
}
