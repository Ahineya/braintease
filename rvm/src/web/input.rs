//! Browser events arrive explicitly in bounded batches.
use crate::{devices::DeviceState, display_rgb565::RGB565Display};
pub fn poll_display_input(_devices: &mut DeviceState, _display: &RGB565Display) {}
pub fn sync_display_input(_devices: &DeviceState, _display: &RGB565Display) {}
