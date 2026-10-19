#![no_std]
include!(concat!(env!("OUT_DIR"), "/generated.rs"));

pub trait Combine {
    fn combine(self, other: Self) -> Self;
}
impl Combine for u32 {
    fn combine(self, other: Self) -> Self {
        self + other
    }
}

#[inline(never)]
pub fn apply<T: Combine>(left: T, right: T) -> T {
    left.combine(right)
}

#[inline(never)]
pub fn checksum(bytes: &[u8]) -> u32 {
    let mut result = BUILD_OFFSET;
    for index in 0..bytes.len() {
        result = apply(result, bytes[index] as u32);
    }
    result
}

pub fn message() -> &'static str {
    #[cfg(feature = "loud")]
    {
        "RUSTCELL_CARGO_LOUD\n"
    }
    #[cfg(not(feature = "loud"))]
    {
        "RUSTCELL_CARGO_BASE\n"
    }
}
