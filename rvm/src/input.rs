//! Shared live input collection. Polling never advances logical time.
use crate::{devices::DeviceState, display_rgb565::RGB565Display, input_events::*};

pub fn sync_display_input(devices: &DeviceState, display: &RGB565Display) {
    display.get_state().lock().unwrap().input.pointer_mode = devices.input.pointer_mode;
}

pub fn poll_display_input(devices: &mut DeviceState, display: &RGB565Display) {
    let shared = display.get_state();
    let mut state = shared.lock().unwrap();
    state.input.pointer_mode = devices.input.pointer_mode;
    if state.input.overflow {
        devices.mark_input_overflow();
        // Establish a fresh snapshot boundary even when all host transitions
        // could not be forwarded. Consumers must resynchronize after overflow.
        devices.input.sequence = devices.input.sequence.wrapping_add(1);
        devices.input.held = state.input.held;
        devices.input.buttons = state.input.buttons;
        devices.input.x = state.input.x;
        devices.input.y = state.input.y;
        devices.input.inside = state.input.inside;
        devices.input.focused = state.input.focused;
        devices.input.captured = state.input.captured;
        state.input.events.clear();
        state.input.overflow = false;
    } else {
        let events = state.input.events.drain(..).filter_map(|e| match e.kind {
            EVENT_KEY => Some(InputTransition::Key {
                key: e.code,
                down: e.flags & FLAG_DOWN != 0,
            }),
            EVENT_MOTION if e.flags & FLAG_RELATIVE != 0 => {
                Some(InputTransition::Relative { dx: e.dx, dy: e.dy })
            }
            EVENT_MOTION => Some(InputTransition::Motion { x: e.x, y: e.y }),
            EVENT_BUTTON => Some(InputTransition::Button {
                button: e.code,
                down: e.flags & FLAG_DOWN != 0,
                x: e.x,
                y: e.y,
            }),
            EVENT_WHEEL => Some(InputTransition::Wheel {
                x: e.wheel_x,
                y: e.wheel_y,
            }),
            EVENT_ENTER => Some(InputTransition::Enter {
                inside: e.flags & FLAG_INSIDE != 0,
            }),
            EVENT_FOCUS => Some(InputTransition::Focus {
                focused: e.flags & FLAG_FOCUSED != 0,
            }),
            EVENT_CAPTURE => Some(InputTransition::Capture {
                captured: e.flags & FLAG_CAPTURED != 0,
            }),
            _ => None,
        });
        devices.accept_events(events);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn live_mouse_and_keys_poll_at_tick_zero_and_preserve_checkpoint() {
        let display = RGB565Display::new();
        let mut devices = DeviceState::new(35).unwrap();
        devices.input.configure(POINTER_RELATIVE);
        sync_display_input(&devices, &display);
        {
            let state = display.get_state();
            let mut state = state.lock().unwrap();
            assert_eq!(state.input.pointer_mode, POINTER_RELATIVE);
            for event in [
                InputTransition::Focus { focused: true },
                InputTransition::Motion { x: 12, y: 34 },
                InputTransition::Key { key: 4, down: true },
                InputTransition::Button {
                    button: 1,
                    down: true,
                    x: 12,
                    y: 34,
                },
                InputTransition::Relative { dx: 5, dy: -6 },
            ] {
                state.input_event(event);
            }
        }
        poll_display_input(&mut devices, &display);
        assert_eq!(devices.tick(), 0);
        assert_eq!(
            devices
                .input
                .events
                .iter()
                .map(|e| e.kind)
                .collect::<Vec<_>>(),
            [6, 2, 1, 3, 2]
        );
        assert_eq!(
            (devices.input.x, devices.input.y, devices.input.buttons),
            (17, 28, 2)
        );
        assert!(devices.input.events.iter().all(|e| e.tick == 0));
        let restored = DeviceState::restore_checkpoint(devices.checkpoint()).unwrap();
        assert_eq!(devices.input, restored.input);
        poll_display_input(&mut devices, &display);
        assert_eq!(devices.input.events.len(), 5);
    }
    #[test]
    fn host_overflow_recovers_authoritative_pointer_and_keys() {
        let display = RGB565Display::new();
        let mut devices = DeviceState::new(35).unwrap();
        {
            let shared = display.get_state();
            let mut state = shared.lock().unwrap();
            for n in 0..300 {
                state.input_event(InputTransition::Motion { x: n, y: -n });
            }
            state.set_key(82, true);
            state.input_event(InputTransition::Button {
                button: 0,
                down: true,
                x: -90000,
                y: 80000,
            });
        }
        poll_display_input(&mut devices, &display);
        assert!(devices.input.overflow && devices.held(82));
        assert_eq!(
            (devices.input.x, devices.input.y, devices.input.buttons),
            (-90000, 80000, 1)
        );
        assert!(devices.input.events.is_empty());
        assert_eq!(devices.input.snapshot_cells()[2], 1);
        assert!(!devices.input.overflow);
        poll_display_input(&mut devices, &display);
        assert!(!devices.input.overflow);
    }
}
