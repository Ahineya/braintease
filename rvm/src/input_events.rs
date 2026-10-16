//! Ordered keyboard/pointer input, independent of window APIs and clock grants.
use serde::{Deserialize, Serialize};
use std::collections::VecDeque;

pub const EVENT_CELLS: usize = 20;
pub const STATE_CELLS: usize = 28;
pub const MAX_BATCH: usize = 64;
pub const QUEUE_CAPACITY: usize = 256;
pub const POINTER_ABSOLUTE: u16 = 0;
pub const POINTER_RELATIVE: u16 = 1;
pub const FLAG_DOWN: u16 = 1;
pub const FLAG_RELATIVE: u16 = 2;
pub const FLAG_INSIDE: u16 = 4;
pub const FLAG_FOCUSED: u16 = 8;
pub const FLAG_CAPTURED: u16 = 16;
pub const EVENT_KEY: u16 = 1;
pub const EVENT_MOTION: u16 = 2;
pub const EVENT_BUTTON: u16 = 3;
pub const EVENT_WHEEL: u16 = 4;
pub const EVENT_ENTER: u16 = 5;
pub const EVENT_FOCUS: u16 = 6;
pub const EVENT_CAPTURE: u16 = 7;

/// Absolute coordinates count guest framebuffer pixels; relative deltas count raw
/// device units (positive X right, positive Y down). Wheel values are signed 16.16 lines:
/// positive X scrolls right and positive Y scrolls up. Button IDs are 0..15
/// (left, right, middle, then host extra buttons).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum InputTransition {
    Key {
        key: u16,
        down: bool,
    },
    Motion {
        x: i32,
        y: i32,
    },
    Relative {
        dx: i32,
        dy: i32,
    },
    Button {
        button: u16,
        down: bool,
        x: i32,
        y: i32,
    },
    Wheel {
        x: i32,
        y: i32,
    },
    Enter {
        inside: bool,
    },
    Focus {
        focused: bool,
    },
    Capture {
        captured: bool,
    },
}
impl InputTransition {
    pub fn valid(self) -> bool {
        match self {
            Self::Key { key, .. } => key < 256,
            Self::Button { button, .. } => button < 16,
            _ => true,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct InputEvent {
    pub kind: u16,
    pub flags: u16,
    pub code: u16,
    pub buttons: u16,
    pub sequence: u32,
    pub tick: u64,
    pub x: i32,
    pub y: i32,
    pub dx: i32,
    pub dy: i32,
    pub wheel_x: i32,
    pub wheel_y: i32,
}
impl InputEvent {
    pub fn cells(self) -> [u16; EVENT_CELLS] {
        let mut words = [0; EVENT_CELLS];
        words[..4].copy_from_slice(&[self.kind, self.flags, self.code, self.buttons]);
        for (i, value) in [
            self.sequence,
            self.tick as u32,
            self.x as u32,
            self.y as u32,
            self.dx as u32,
            self.dy as u32,
            self.wheel_x as u32,
            self.wheel_y as u32,
        ]
        .into_iter()
        .enumerate()
        {
            words[4 + i * 2] = value as u16;
            words[5 + i * 2] = (value >> 16) as u16;
        }
        words
    }
    pub fn valid(&self) -> bool {
        matches!(self.kind, EVENT_KEY..=EVENT_CAPTURE)
            && self.flags & !31 == 0
            && (self.kind != EVENT_KEY || self.code < 256)
            && (self.kind != EVENT_BUTTON || self.code < 16)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct InputState {
    pub held: [u16; 16],
    pub buttons: u16,
    pub x: i32,
    pub y: i32,
    pub inside: bool,
    pub focused: bool,
    pub captured: bool,
    /// Requested mode. Capture is acknowledged separately by the window host.
    pub pointer_mode: u16,
    pub sequence: u32,
    pub events: VecDeque<InputEvent>,
    pub overflow: bool,
    pub overflow_seen: bool,
}
impl Default for InputState {
    fn default() -> Self {
        Self {
            held: [0; 16],
            buttons: 0,
            x: 0,
            y: 0,
            inside: false,
            focused: false,
            captured: false,
            pointer_mode: POINTER_ABSOLUTE,
            sequence: 0,
            events: VecDeque::new(),
            overflow: false,
            overflow_seen: false,
        }
    }
}
impl InputState {
    pub fn held(&self, key: u16) -> bool {
        key < 256 && self.held[usize::from(key) / 16] & (1 << (key % 16)) != 0
    }
    pub fn mark_overflow(&mut self) {
        self.overflow = true;
        self.overflow_seen = true;
    }
    pub fn flags(&self) -> u16 {
        u16::from(self.inside) * FLAG_INSIDE
            | u16::from(self.focused) * FLAG_FOCUSED
            | u16::from(self.captured) * FLAG_CAPTURED
    }
    pub fn configure(&mut self, mode: u16) -> bool {
        if mode > POINTER_RELATIVE {
            return false;
        }
        self.pointer_mode = mode;
        true
    }
    pub fn apply(&mut self, transition: InputTransition, tick: u64) {
        if !transition.valid() {
            return;
        }
        let mut event = InputEvent {
            kind: 0,
            flags: 0,
            code: 0,
            buttons: 0,
            sequence: 0,
            tick,
            x: self.x,
            y: self.y,
            dx: 0,
            dy: 0,
            wheel_x: 0,
            wheel_y: 0,
        };
        match transition {
            InputTransition::Key { key, down } => {
                if self.held(key) == down {
                    return;
                }
                let bit = 1 << (key % 16);
                let word = &mut self.held[usize::from(key) / 16];
                if down {
                    *word |= bit;
                } else {
                    *word &= !bit;
                }
                event.kind = EVENT_KEY;
                event.code = key;
                event.flags = u16::from(down) * FLAG_DOWN;
            }
            InputTransition::Motion { x, y } => {
                if self.x == x && self.y == y {
                    return;
                }
                event.kind = EVENT_MOTION;
                event.dx = x.saturating_sub(self.x);
                event.dy = y.saturating_sub(self.y);
                self.x = x;
                self.y = y;
            }
            InputTransition::Relative { dx, dy } => {
                if dx == 0 && dy == 0 {
                    return;
                }
                event.kind = EVENT_MOTION;
                event.flags = FLAG_RELATIVE;
                event.dx = dx;
                event.dy = dy;
                self.x = self.x.saturating_add(dx);
                self.y = self.y.saturating_add(dy);
            }
            InputTransition::Button { button, down, x, y } => {
                let bit = 1 << button;
                if (self.buttons & bit != 0) == down {
                    return;
                }
                if down {
                    self.buttons |= bit;
                } else {
                    self.buttons &= !bit;
                }
                self.x = x;
                self.y = y;
                event.kind = EVENT_BUTTON;
                event.code = button;
                event.flags = u16::from(down) * FLAG_DOWN;
            }
            InputTransition::Wheel { x, y } => {
                if x == 0 && y == 0 {
                    return;
                }
                event.kind = EVENT_WHEEL;
                event.wheel_x = x;
                event.wheel_y = y;
            }
            InputTransition::Enter { inside } => {
                if self.inside == inside {
                    return;
                }
                self.inside = inside;
                event.kind = EVENT_ENTER;
            }
            InputTransition::Focus { focused } => {
                if self.focused == focused
                    && (focused
                        || (self.held.iter().all(|w| *w == 0)
                            && self.buttons == 0
                            && !self.captured))
                {
                    return;
                }
                self.focused = focused;
                event.kind = EVENT_FOCUS;
            }
            InputTransition::Capture { captured } => {
                if self.captured == captured {
                    return;
                }
                self.captured = captured;
                event.kind = EVENT_CAPTURE;
            }
        }
        self.sequence = self.sequence.wrapping_add(1);
        event.sequence = self.sequence;
        event.buttons = self.buttons;
        event.x = self.x;
        event.y = self.y;
        event.flags |= self.flags();
        if self.events.len() == QUEUE_CAPACITY {
            self.mark_overflow();
        } else {
            self.events.push_back(event);
        }
        // Focus loss must release every held input, including in replay and
        // headless hosts; lost transitions are recoverable through the snapshot.
        if matches!(transition, InputTransition::Focus { focused: false }) {
            for key in 0..256 {
                if self.held(key) {
                    self.apply(InputTransition::Key { key, down: false }, tick);
                }
            }
            for button in 0..16 {
                if self.buttons & (1 << button) != 0 {
                    self.apply(
                        InputTransition::Button {
                            button,
                            down: false,
                            x: self.x,
                            y: self.y,
                        },
                        tick,
                    );
                }
            }
            self.apply(InputTransition::Capture { captured: false }, tick);
        }
    }
    pub fn snapshot_cells(&mut self) -> [u16; STATE_CELLS] {
        let mut cells = [0; STATE_CELLS];
        cells[..8].copy_from_slice(&[
            1,
            STATE_CELLS as u16,
            u16::from(self.overflow),
            self.sequence as u16,
            (self.sequence >> 16) as u16,
            self.pointer_mode,
            self.buttons,
            self.flags(),
        ]);
        cells[8] = self.x as u16;
        cells[9] = (self.x as u32 >> 16) as u16;
        cells[10] = self.y as u16;
        cells[11] = (self.y as u32 >> 16) as u16;
        cells[12..].copy_from_slice(&self.held);
        if self.overflow {
            self.events.clear();
        }
        self.overflow = false;
        cells
    }
    pub fn validate(&self, tick: u64) -> bool {
        if self.pointer_mode > POINTER_RELATIVE
            || self.events.len() > QUEUE_CAPACITY
            || self.overflow && !self.overflow_seen
        {
            return false;
        }
        let mut previous: Option<(u32, u64)> = None;
        for event in &self.events {
            if !event.valid()
                || event.tick > tick
                || previous.is_some_and(|(seq, t)| {
                    event.tick < t
                        || (event.sequence.wrapping_sub(seq) == 0
                            || event.sequence.wrapping_sub(seq) >= 0x80000000)
                })
            {
                return false;
            }
            if self.sequence.wrapping_sub(event.sequence) >= 0x80000000 {
                return false;
            }
            previous = Some((event.sequence, event.tick));
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn mixed_events_preserve_order_signed_values_and_click_coordinates() {
        let mut s = InputState::default();
        for event in [
            InputTransition::Focus { focused: true },
            InputTransition::Key {
                key: 26,
                down: true,
            },
            InputTransition::Motion {
                x: -70000,
                y: 90000,
            },
            InputTransition::Button {
                button: 0,
                down: true,
                x: 123456,
                y: -345678,
            },
            InputTransition::Relative { dx: -2, dy: 3 },
            InputTransition::Wheel {
                x: -32768,
                y: 98304,
            },
        ] {
            s.apply(event, 7);
        }
        assert_eq!(
            s.events.iter().map(|e| e.kind).collect::<Vec<_>>(),
            [6, 1, 2, 3, 2, 4]
        );
        let click = s.events[3];
        assert_eq!((click.x, click.y, click.buttons), (123456, -345678, 1));
        assert_eq!((s.x, s.y), (123454, -345675));
        assert_eq!(s.events[4].flags, FLAG_RELATIVE | FLAG_FOCUSED);
        let words = s.events[5].cells();
        let signed = |i: usize| (u32::from(words[i]) | (u32::from(words[i + 1]) << 16)) as i32;
        assert_eq!(
            (signed(8), signed(10), signed(16), signed(18)),
            (123454, -345675, -32768, 98304)
        );
        assert!(s
            .events
            .iter()
            .enumerate()
            .all(|(i, e)| e.tick == 7 && e.sequence == i as u32 + 1));
        assert!(s.validate(7));
        assert!(!s.validate(6));
    }
    #[test]
    fn focus_loss_releases_everything_even_without_prior_focus_event() {
        let mut s = InputState::default();
        s.apply(
            InputTransition::Key {
                key: 224,
                down: true,
            },
            0,
        );
        s.apply(
            InputTransition::Button {
                button: 2,
                down: true,
                x: 4,
                y: 5,
            },
            0,
        );
        s.apply(InputTransition::Capture { captured: true }, 0);
        s.apply(InputTransition::Focus { focused: false }, 0);
        assert_eq!((s.held(224), s.buttons, s.captured), (false, 0, false));
        assert_eq!(
            s.events.iter().map(|e| e.kind).collect::<Vec<_>>(),
            [1, 3, 7, 6, 1, 3, 7]
        );
        let seq = s.sequence;
        s.apply(InputTransition::Focus { focused: false }, 0);
        assert_eq!(s.sequence, seq);
    }
    #[test]
    fn snapshot_only_discards_queue_after_overflow_and_records_final_state() {
        let mut s = InputState::default();
        s.apply(InputTransition::Key { key: 4, down: true }, 0);
        assert_eq!(s.snapshot_cells()[12], 16);
        assert_eq!(s.events.len(), 1);
        for n in 0..QUEUE_CAPACITY {
            s.apply(
                InputTransition::Motion {
                    x: n as i32 + 1,
                    y: -1,
                },
                1,
            );
        }
        s.apply(
            InputTransition::Key {
                key: 4,
                down: false,
            },
            1,
        );
        s.apply(
            InputTransition::Button {
                button: 1,
                down: true,
                x: 90000,
                y: -100000,
            },
            1,
        );
        assert!(s.overflow && s.overflow_seen);
        assert_eq!(s.events.len(), QUEUE_CAPACITY);
        let cells = s.snapshot_cells();
        assert_eq!((cells[2], cells[6], cells[12]), (1, 2, 0));
        assert_eq!(u32::from(cells[8]) | (u32::from(cells[9]) << 16), 90000);
        assert!(!s.overflow && s.events.is_empty());
        assert!(s.overflow_seen && s.validate(1));
    }
    #[test]
    fn checkpoint_and_sequence_wrap_preserve_pending_mouse_events() {
        let mut s = InputState::default();
        s.sequence = u32::MAX - 1;
        s.configure(POINTER_RELATIVE);
        s.apply(InputTransition::Relative { dx: 9, dy: -8 }, 3);
        s.apply(InputTransition::Wheel { x: 0, y: -65536 }, 3);
        assert_eq!(
            s.events.iter().map(|e| e.sequence).collect::<Vec<_>>(),
            [u32::MAX, 0]
        );
        assert!(s.validate(3));
        let mut restored: InputState =
            serde_json::from_slice(&serde_json::to_vec(&s).unwrap()).unwrap();
        assert_eq!(s, restored);
        restored.events.back_mut().unwrap().sequence = 3;
        assert!(!restored.validate(3));
        s.events.back_mut().unwrap().tick = 2;
        assert!(!s.validate(3));
    }
}
