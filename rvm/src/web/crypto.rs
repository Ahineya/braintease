//! The offline browser profile does not advertise a crypto coprocessor.
use crate::devices::{GuestMemory, STATUS_UNSUPPORTED};
pub const COMMAND: u16 = 13;
pub fn execute<M: GuestMemory + ?Sized>(
    _m: &mut M,
    _bank: u16,
    _range: std::ops::Range<usize>,
    _words: &[u16],
) -> Result<(), u16> {
    Err(STATUS_UNSUPPORTED)
}
