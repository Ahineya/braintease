//! Explicit offline NIC. No host sockets or implicit network relay.
use crate::devices::{GuestMemory, STATUS_UNSUPPORTED};
pub const CAP_NETWORK: u16 = 0;
#[derive(Debug, Default, Clone, serde::Serialize, serde::Deserialize)]
pub struct NetworkDevice;
pub fn descriptor_length(command: u16) -> Option<usize> {
    match command {
        0x100 => Some(18),
        0x101 => Some(8),
        0x102 => Some(9),
        0x103 => Some(27),
        _ => None,
    }
}
impl NetworkDevice {
    pub fn reset(&self) {}
    pub fn validate_checkpoint(&self) -> Result<(), String> {
        Ok(())
    }
    pub fn execute<M: GuestMemory + ?Sized>(
        &mut self,
        _c: u16,
        _m: &mut M,
        _r: std::ops::Range<usize>,
        _b: u16,
    ) -> Result<(), u16> {
        Err(STATUS_UNSUPPORTED)
    }
}
