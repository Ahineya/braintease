//! Shared live keyboard collection for VM, BF and native hosts. Logical clock
//! advancement and scheduled replay input are deliberately separate.
use crate::{
    devices::{DeviceState, KeyTransition},
    display_rgb565::RGB565Display,
};

pub fn poll_display_input(devices: &mut DeviceState, display: &RGB565Display) {
    let shared = display.get_state();
    let mut state = shared.lock().unwrap();
    if state.key_overflow {
        devices.mark_input_overflow();
        // Lost transitions cannot be replayed safely. The held bitmap remains
        // authoritative; INPUT_STATE will acknowledge the overflow to the guest.
        state.pending_keys.clear();
        devices.accept_input(
            state
                .keys_held
                .iter()
                .copied()
                .enumerate()
                .map(|(key, down)| KeyTransition {
                    key: key as u16,
                    down,
                }),
        );
        state.key_overflow = false;
    } else {
        devices.accept_input(
            state
                .pending_keys
                .drain(..)
                .map(|(key, down)| KeyTransition { key, down }),
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn live_events_are_ordered_and_checkpointable_at_tick_zero() {
        let mut devices = DeviceState::default();
        let display = RGB565Display::new();
        {
            let shared = display.get_state();
            let mut state = shared.lock().unwrap();
            state.set_key(4, true);
            state.set_key(4, false);
            state.set_key(5, true);
        }
        poll_display_input(&mut devices, &display);
        assert_eq!(devices.tick(), 0);
        assert!(!devices.held(4));
        assert!(devices.held(5));
        let snapshot = serde_json::to_value(devices.checkpoint()).unwrap();
        assert_eq!(
            snapshot["events"],
            serde_json::json!([
                {"key": 4, "down": true, "tick": 0},
                {"key": 4, "down": false, "tick": 0},
                {"key": 5, "down": true, "tick": 0}
            ])
        );
        let restored = DeviceState::restore_checkpoint(devices.checkpoint()).unwrap();
        assert_eq!(
            serde_json::to_value(restored).unwrap(),
            serde_json::to_value(devices.checkpoint()).unwrap()
        );
        // Polling again does not duplicate transitions.
        let before = serde_json::to_value(devices.checkpoint()).unwrap();
        poll_display_input(&mut devices, &display);
        assert_eq!(before, serde_json::to_value(devices.checkpoint()).unwrap());
        let shared = display.get_state();
        assert!(shared.lock().unwrap().pending_keys.is_empty());
    }
    #[test]
    fn host_queue_overflow_recovers_held_keys_without_advancing_time() {
        let mut devices = DeviceState::default();
        let display = RGB565Display::new();
        {
            let shared = display.get_state();
            let mut state = shared.lock().unwrap();
            for i in 0..4097 {
                state.set_key(4, i % 2 == 0);
            }
            state.set_key(5, true);
            assert!(state.key_overflow);
        }
        poll_display_input(&mut devices, &display);
        assert_eq!(devices.tick(), 0);
        assert!(devices.held(4) && devices.held(5));
        assert!(devices.input_overflowed());
        let shared = display.get_state();
        let state = shared.lock().unwrap();
        assert!(!state.key_overflow && state.pending_keys.is_empty());
        DeviceState::restore_checkpoint(devices.checkpoint()).unwrap();
    }
}
