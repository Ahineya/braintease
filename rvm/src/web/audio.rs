//! Audio is unavailable in the initial browser profile.
use crate::devices::{GuestMemory, STATUS_UNSUPPORTED};
pub const CAP_PCM: u16 = 0;
pub const CAP_MUSIC: u16 = 0;
#[derive(Debug, Default, Clone, serde::Serialize, serde::Deserialize)]
pub struct AudioDevice;
pub fn descriptor_length(command: u16) -> Option<usize> {
    match command {
        0x200 => Some(8),
        0x201 => Some(10),
        0x202 => Some(16),
        0x203 => Some(4),
        0x204 | 0x205 => Some(6),
        0x206 => Some(12),
        0x210 | 0x211 => Some(8),
        0x212 => Some(4),
        0x213 => Some(10),
        _ => None,
    }
}
impl AudioDevice {
    pub fn reset(&mut self) {}
    pub fn supported(&self) -> bool {
        false
    }
    pub fn validate(&self, _hz: u16) -> Result<(), String> {
        Ok(())
    }
    pub fn error(&self) -> Option<&str> {
        None
    }
    pub fn advance_tick(&mut self, _hz: u16) -> Result<(), u16> {
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
