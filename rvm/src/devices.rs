//! Versioned, synchronous device commands shared by VM implementations.
//!
//! Device descriptors and framebuffers live in ordinary guest RAM. The five
//! mailbox registers are the only new MMIO addresses. `NEXT_TICK` suspends the
//! guest at an instruction boundary until its runner grants exactly one tick.

pub use crate::input_events::{InputEvent, InputState, InputTransition};
use crate::input_events::{EVENT_CELLS, MAX_BATCH, STATE_CELLS};
use serde::{Deserialize, Serialize};
use std::ops::Range;
use std::path::Path;

pub const ABI_VERSION: u16 = 1;
pub const REGISTER_DESCRIPTOR_BANK: usize = 27;
pub const REGISTER_DESCRIPTOR_OFFSET: usize = 28;
pub const REGISTER_COMMAND: usize = 29;
pub const REGISTER_STATUS: usize = 30;
pub const REGISTER_VERSION: usize = 31;
pub const DISPLAY_MODE_EXTENDED: u16 = 4;
/// INPUT_READ/INPUT_STATE collect live input without a clock grant.
pub const CAP_INPUT_POLL: u16 = 0x0010;
pub const CAP_HOST_CLOCK: u16 = 0x1000;
pub const CAP_HOST_IDLE: u16 = 0x2000;
pub const CAP_HOST_TERMINAL: u16 = 0x4000;
pub const CAP_HOST_RANDOM: u16 = 0x8000;
pub const CAPABILITIES: u16 = 15
    | crate::bulk::CAP_MEMORY
    | CAP_INPUT_POLL
    | CAP_HOST_CLOCK
    | CAP_HOST_IDLE
    | CAP_HOST_TERMINAL
    | CAP_HOST_RANDOM
    | crate::network::CAP_NETWORK
    | 0x0040
    | crate::audio::CAP_PCM
    | crate::audio::CAP_MUSIC;
pub const KEY_COUNT: usize = 256;
pub const INPUT_QUEUE_CAPACITY: usize = 256;

pub const COMMAND_PRESENT: u16 = 1;
pub const COMMAND_CAPABILITIES: u16 = 2;
pub const COMMAND_NEXT_TICK: u16 = 3;
pub const COMMAND_GET_TIME: u16 = 4;
pub const COMMAND_INPUT_READ: u16 = 5;
pub const COMMAND_INPUT_STATE: u16 = 6;
pub const COMMAND_INPUT_CONFIGURE: u16 = 7;
pub const COMMAND_HOST_MONOTONIC: u16 = 8;
pub const COMMAND_HOST_REALTIME: u16 = 9;
pub const COMMAND_HOST_IDLE: u16 = 10;
pub const COMMAND_HOST_TERMINAL: u16 = 11;
pub const COMMAND_HOST_RANDOM: u16 = 12;
pub const COMMAND_TTY_WRITE: u16 = crate::terminal::COMMAND;
/// Short, synchronous host waits keep cooperative reactors responsive.
pub const HOST_IDLE_MAX_MS: u16 = 10;

pub const STATUS_OK: u16 = 0;
pub const STATUS_UNKNOWN_COMMAND: u16 = 1;
pub const STATUS_VERSION: u16 = 2;
pub const STATUS_BAD_DESCRIPTOR: u16 = 3;
pub const STATUS_BOUNDS: u16 = 4;
pub const STATUS_DISPLAY_MODE: u16 = 5;
pub const STATUS_INPUT_OVERFLOW: u16 = 6;
pub const STATUS_UNSUPPORTED: u16 = 7;
pub const STATUS_WOULD_BLOCK: u16 = 8;
pub const STATUS_BAD_STATE: u16 = 9;
pub const STATUS_IO: u16 = 10;
pub const STATUS_PENDING: u16 = 0x8000;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct KeyTransition {
    pub key: u16,
    pub down: bool,
}

impl From<KeyTransition> for InputTransition {
    fn from(key: KeyTransition) -> Self {
        Self::Key {
            key: key.key,
            down: key.down,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PresentFrame {
    pub width: u16,
    pub height: u16,
    pub pixels: Vec<u16>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ScheduledInputEvent {
    tick: u64,
    transition: InputTransition,
}

/// An ordered replay. Same-tick transitions retain their file order.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct InputScript {
    events: Vec<ScheduledInputEvent>,
}

impl InputScript {
    /// Interpret a replay's positive ticks relative to a restored clock.
    pub fn relative_to(mut self, tick: u64) -> Result<Self, String> {
        for event in &mut self.events {
            event.tick = event
                .tick
                .checked_add(tick)
                .ok_or("relative input script tick overflow")?;
        }
        Ok(self)
    }

    pub fn from_json(json: &str) -> Result<Self, String> {
        let entries: Vec<serde_json::Value> =
            serde_json::from_str(json).map_err(|e| format!("invalid input script: {e}"))?;
        let mut events = Vec::with_capacity(entries.len());
        let mut last_tick = 0;
        for (index, mut entry) in entries.into_iter().enumerate() {
            let object = entry
                .as_object_mut()
                .ok_or("input script event must be an object")?;
            let tick = object
                .remove("tick")
                .and_then(|v| v.as_u64())
                .ok_or("input script event needs a tick")?;
            if tick == 0 || tick < last_tick {
                return Err(format!(
                    "input script event {} must have a positive, nondecreasing tick",
                    index + 1
                ));
            }
            if !object.contains_key("type") {
                object.insert("type".into(), serde_json::json!("key"));
            }
            if let Some(value) = object.get_mut("key") {
                if let Some(name) = value.as_str() {
                    *value = serde_json::json!(
                        key_code(name).ok_or_else(|| format!("unknown key {name:?}"))?
                    );
                }
            }
            let transition: InputTransition = serde_json::from_value(entry)
                .map_err(|e| format!("input script event {}: {e}", index + 1))?;
            if !transition.valid() {
                return Err("input script key/button out of range".into());
            }
            last_tick = tick;
            events.push(ScheduledInputEvent { tick, transition });
        }
        Ok(Self { events })
    }

    pub fn from_path(path: impl AsRef<Path>) -> Result<Self, String> {
        let path = path.as_ref();
        let json = std::fs::read_to_string(path)
            .map_err(|error| format!("cannot read input script {}: {error}", path.display()))?;
        Self::from_json(&json)
    }
}

/// Stable key IDs follow USB HID keyboard usage IDs, independent of host APIs.
/// A key denotes a physical key; modifiers are separate keys, not text input.
pub fn key_code(name: &str) -> Option<u16> {
    if name.trim() == "-" {
        return Some(45);
    }
    let normalized = name
        .trim()
        .to_ascii_uppercase()
        .replace(['_', '-', ' '], "");
    let bytes = normalized.as_bytes();
    if bytes.len() == 1 {
        return match bytes[0] {
            b'A'..=b'Z' => Some(u16::from(bytes[0] - b'A') + 4),
            b'1'..=b'9' => Some(u16::from(bytes[0] - b'1') + 30),
            b'0' => Some(39),
            b'=' => Some(46),
            b'[' => Some(47),
            b']' => Some(48),
            b'\\' => Some(49),
            b';' => Some(51),
            b'\'' => Some(52),
            b'`' => Some(53),
            b',' => Some(54),
            b'.' => Some(55),
            b'/' => Some(56),
            _ => None,
        };
    }
    if let Some(number) = normalized
        .strip_prefix('F')
        .and_then(|value| value.parse::<u16>().ok())
    {
        return match number {
            1..=12 => Some(57 + number),
            13..=24 => Some(91 + number),
            _ => None,
        };
    }
    Some(match normalized.as_str() {
        "ENTER" | "RETURN" => 40,
        "ESC" | "ESCAPE" => 41,
        "BACKSPACE" => 42,
        "TAB" => 43,
        "SPACE" | "SPACEBAR" => 44,
        "MINUS" => 45,
        "EQUAL" | "EQUALS" => 46,
        "LEFTBRACKET" | "LBRACKET" => 47,
        "RIGHTBRACKET" | "RBRACKET" => 48,
        "BACKSLASH" => 49,
        "NONUSHASH" => 50,
        "SEMICOLON" => 51,
        "APOSTROPHE" | "QUOTE" => 52,
        "GRAVE" | "BACKTICK" => 53,
        "COMMA" => 54,
        "PERIOD" | "DOT" => 55,
        "SLASH" => 56,
        "CAPSLOCK" => 57,
        "PRINTSCREEN" | "SYSRQ" => 70,
        "SCROLLLOCK" => 71,
        "PAUSE" | "BREAK" => 72,
        "INSERT" => 73,
        "HOME" => 74,
        "PAGEUP" | "PGUP" => 75,
        "DELETE" | "DEL" => 76,
        "END" => 77,
        "PAGEDOWN" | "PGDOWN" | "PGDN" => 78,
        "RIGHT" | "ARROWRIGHT" => 79,
        "LEFT" | "ARROWLEFT" => 80,
        "DOWN" | "ARROWDOWN" => 81,
        "UP" | "ARROWUP" => 82,
        "NUMLOCK" => 83,
        "KPDIVIDE" | "NUMPADDIVIDE" => 84,
        "KPMULTIPLY" | "NUMPADMULTIPLY" => 85,
        "KPSUBTRACT" | "NUMPADSUBTRACT" | "KPMINUS" => 86,
        "KPADD" | "NUMPADADD" | "KPPLUS" => 87,
        "KPENTER" | "NUMPADENTER" => 88,
        "KP1" | "NUMPAD1" => 89,
        "KP2" | "NUMPAD2" => 90,
        "KP3" | "NUMPAD3" => 91,
        "KP4" | "NUMPAD4" => 92,
        "KP5" | "NUMPAD5" => 93,
        "KP6" | "NUMPAD6" => 94,
        "KP7" | "NUMPAD7" => 95,
        "KP8" | "NUMPAD8" => 96,
        "KP9" | "NUMPAD9" => 97,
        "KP0" | "NUMPAD0" => 98,
        "KPDECIMAL" | "KPPERIOD" | "NUMPADDECIMAL" | "NUMPADPERIOD" => 99,
        "NONUSBACKSLASH" => 100,
        "APPLICATION" | "MENU" | "CONTEXTMENU" => 101,
        "POWER" => 102,
        "KPEQUAL" | "NUMPADEQUAL" => 103,
        "EXECUTE" => 116,
        "HELP" => 117,
        "SELECT" => 119,
        "STOP" => 120,
        "AGAIN" => 121,
        "UNDO" => 122,
        "CUT" => 123,
        "COPY" => 124,
        "PASTE" => 125,
        "FIND" => 126,
        "MUTE" => 127,
        "VOLUMEUP" => 128,
        "VOLUMEDOWN" => 129,
        "LEFTCTRL" | "LCTRL" | "LEFTCONTROL" | "CTRL" | "CONTROL" => 224,
        "LEFTSHIFT" | "LSHIFT" | "SHIFT" => 225,
        "LEFTALT" | "LALT" | "ALT" => 226,
        "LEFTGUI" | "LGUI" | "LEFTMETA" | "LEFTSUPER" | "META" | "SUPER" | "COMMAND" => 227,
        "RIGHTCTRL" | "RCTRL" | "RIGHTCONTROL" => 228,
        "RIGHTSHIFT" | "RSHIFT" => 229,
        "RIGHTALT" | "RALT" | "ALTGR" => 230,
        "RIGHTGUI" | "RGUI" | "RIGHTMETA" | "RIGHTSUPER" => 231,
        _ => return None,
    })
}

/// Logical guest RAM, independent of a backend's physical tape layout.
/// Callers validate ranges before accessing cells; implementations must expose
/// exactly `len()` addressable cells and must not route accesses through MMIO.
pub trait GuestMemory {
    fn len(&self) -> usize;
    fn read(&self, index: usize) -> u16;
    fn write(&mut self, index: usize, value: u16);
    /// Optional runner transcript of raw bytes accepted by console DMA.
    fn record_console_output(&mut self, _bytes: &[u8]) {}
    /// A runner which owns console delivery may accept the entire batch here.
    /// Returning false must have no effects; the device then writes stdout.
    fn buffer_console_output(&mut self, _bytes: &[u8]) -> bool { false }
    /// Separate Harvard instruction space. Zero means this adapter cannot upload code.
    fn code_capacity(&self, _bank_size: u16) -> usize {
        0
    }
    /// The caller validates the complete range first. Implementations must grow
    /// with NOPs like STORC and commit the entire upload or return an error.
    fn write_code(
        &mut self,
        _bank_size: u16,
        _start: usize,
        _code: &[[u16; 4]],
    ) -> Result<(), u16> {
        Err(STATUS_UNSUPPORTED)
    }
    fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

impl GuestMemory for [u16] {
    fn len(&self) -> usize {
        <[u16]>::len(self)
    }
    fn read(&self, index: usize) -> u16 {
        self[index]
    }
    fn write(&mut self, index: usize, value: u16) {
        self[index] = value;
    }
}

impl GuestMemory for Vec<u16> {
    fn len(&self) -> usize {
        Vec::len(self)
    }
    fn read(&self, index: usize) -> u16 {
        self[index]
    }
    fn write(&mut self, index: usize, value: u16) {
        self[index] = value;
    }
}

/// Validate a contiguous cell span, including bank crossings, in ordinary RAM.
pub fn guest_range(
    memory_len: usize,
    bank_size: u16,
    bank: u16,
    offset: u16,
    length: usize,
) -> Result<Range<usize>, u16> {
    let bank_size = usize::from(bank_size);
    if bank_size == 0 || usize::from(offset) >= bank_size {
        return Err(STATUS_BOUNDS);
    }
    let start = usize::from(bank)
        .checked_mul(bank_size)
        .and_then(|base| base.checked_add(usize::from(offset)))
        .ok_or(STATUS_BOUNDS)?;
    let end = start.checked_add(length).ok_or(STATUS_BOUNDS)?;
    let address_limit = bank_size.checked_mul(65536).ok_or(STATUS_BOUNDS)?;
    if start < 32 || end > memory_len || end > address_limit {
        return Err(STATUS_BOUNDS);
    }
    Ok(start..end)
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct DescriptorPointer {
    bank: u16,
    offset: u16,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct DeviceState {
    #[serde(default)]
    host_clock: crate::host_clock::HostClock,
    #[serde(default)]
    pub network: crate::network::NetworkDevice,
    #[serde(default)]
    pub audio: crate::audio::AudioDevice,
    descriptor: DescriptorPointer,
    command: u16,
    status: u16,
    pending_tick: Option<DescriptorPointer>,
    tick_hz: u16,
    tick: u64,
    pub input: InputState,
    script: InputScript,
    script_position: usize,
}

impl Default for DeviceState {
    fn default() -> Self {
        Self::new(35).expect("the default tick rate is nonzero")
    }
}

impl DeviceState {
    /// Freeze elapsed host time. Restore resumes it, excluding checkpoint downtime.
    pub fn checkpoint(&self) -> Self {
        let mut snapshot = self.clone();
        snapshot.host_clock = self.host_clock.checkpoint();
        snapshot
    }

    pub fn restore_checkpoint(mut snapshot: Self) -> Result<Self, String> {
        snapshot.network.validate_checkpoint()?;
        snapshot.audio.validate(snapshot.tick_hz)?;
        if snapshot.tick_hz == 0
            || !snapshot.input.validate(snapshot.tick)
            || snapshot.script_position > snapshot.script.events.len()
            || !matches!(snapshot.status, 0..=10 | STATUS_PENDING)
            || snapshot.pending_tick.is_some() != (snapshot.status == STATUS_PENDING)
            || snapshot.pending_tick.is_some() && snapshot.command != COMMAND_NEXT_TICK
        {
            return Err("invalid device checkpoint state".into());
        }
        let mut previous = 0;
        for (index, event) in snapshot.script.events.iter().enumerate() {
            if !event.transition.valid()
                || event.tick == 0
                || event.tick < previous
                || (index < snapshot.script_position) != (event.tick <= snapshot.tick)
            {
                return Err("invalid device checkpoint input script/cursor".into());
            }
            previous = event.tick;
        }
        snapshot.host_clock.resume();
        Ok(snapshot)
    }

    pub fn new(tick_hz: u16) -> Result<Self, String> {
        if tick_hz == 0 {
            return Err("virtual tick rate must be greater than zero".to_string());
        }
        Ok(Self {
            host_clock: crate::host_clock::HostClock::default(),
            network: crate::network::NetworkDevice::default(),
            audio: crate::audio::AudioDevice::default(),
            descriptor: DescriptorPointer { bank: 0, offset: 0 },
            command: 0,
            status: STATUS_OK,
            pending_tick: None,
            tick_hz,
            tick: 0,
            input: InputState::default(),
            script: InputScript::default(),
            script_position: 0,
        })
    }

    pub fn reset(&mut self) {
        let script = self.script.clone();
        let mut audio = std::mem::take(&mut self.audio);
        audio.reset();
        let network = std::mem::take(&mut self.network);
        network.reset();
        *self = Self::new(self.tick_hz).expect("an initialized tick rate is nonzero");
        self.script = script;
        self.network = network;
        self.audio = audio;
    }

    pub fn set_tick_hz(&mut self, tick_hz: u16) -> Result<(), String> {
        if tick_hz == 0 {
            return Err("virtual tick rate must be greater than zero".to_string());
        }
        if self.tick != 0 || self.has_pending_tick() {
            return Err("virtual tick rate cannot change after the clock starts".to_string());
        }
        self.tick_hz = tick_hz;
        Ok(())
    }

    pub fn set_script(&mut self, script: InputScript) -> Result<(), String> {
        if script
            .events
            .first()
            .is_some_and(|event| event.tick <= self.tick)
        {
            return Err("input script contains an event at or before the current tick".to_string());
        }
        self.script = script;
        self.script_position = 0;
        Ok(())
    }

    pub fn tick_hz(&self) -> u16 {
        self.tick_hz
    }
    pub fn tick(&self) -> u64 {
        self.tick
    }
    pub fn millis(&self) -> u32 {
        ((u128::from(self.tick) * 1000) / u128::from(self.tick_hz)) as u32
    }
    pub fn status(&self) -> u16 {
        self.status
    }
    pub fn has_pending_tick(&self) -> bool {
        self.pending_tick.is_some()
    }
    pub fn input_overflowed(&self) -> bool {
        self.input.overflow
    }
    pub fn overflow_seen(&self) -> bool {
        self.input.overflow_seen
    }
    pub fn held(&self, key: u16) -> bool {
        self.input.held(key)
    }

    pub fn mark_input_overflow(&mut self) {
        self.input.overflow = true;
        self.input.overflow_seen = true;
    }

    pub fn read_register(&self, address: usize) -> Option<u16> {
        match address {
            REGISTER_DESCRIPTOR_BANK => Some(self.descriptor.bank),
            REGISTER_DESCRIPTOR_OFFSET => Some(self.descriptor.offset),
            REGISTER_COMMAND => Some(self.command),
            REGISTER_STATUS => Some(self.status),
            REGISTER_VERSION => Some(ABI_VERSION),
            _ => None,
        }
    }

    /// Start a command. Errors are reported in STATUS and never partially write
    /// a descriptor or present a frame, except irreversible TTY I/O reports its
    /// completed count on STATUS_IO. STATUS and VERSION are read-only.
    pub fn write_register(
        &mut self,
        address: usize,
        value: u16,
        memory: &mut [u16],
        bank_size: u16,
        display_mode: u16,
    ) -> Option<PresentFrame> {
        self.write_register_with_memory(address, value, memory, bank_size, display_mode)
    }

    pub fn write_register_with_memory<M: GuestMemory + ?Sized>(
        &mut self,
        address: usize,
        value: u16,
        memory: &mut M,
        bank_size: u16,
        display_mode: u16,
    ) -> Option<PresentFrame> {
        self.write_register_with_storage(address, value, memory, bank_size, display_mode, None)
    }

    /// Device context uses the same disk cache as the legacy storage ports.
    pub fn write_register_with_storage<M: GuestMemory + ?Sized>(
        &mut self,
        address: usize,
        value: u16,
        memory: &mut M,
        bank_size: u16,
        display_mode: u16,
        storage: Option<&mut crate::vm::Storage>,
    ) -> Option<PresentFrame> {
        match address {
            REGISTER_DESCRIPTOR_BANK => self.descriptor.bank = value,
            REGISTER_DESCRIPTOR_OFFSET => self.descriptor.offset = value,
            REGISTER_COMMAND => {
                if self.has_pending_tick() {
                    self.status = STATUS_PENDING;
                    return None;
                }
                self.command = value;
                match self.execute_command(value, memory, bank_size, display_mode, storage) {
                    Ok(frame) => return frame,
                    Err(status) => self.status = status,
                }
            }
            _ => {}
        }
        None
    }

    fn descriptor_range<M: GuestMemory + ?Sized>(
        pointer: DescriptorPointer,
        memory: &M,
        bank_size: u16,
        length: usize,
    ) -> Result<Range<usize>, u16> {
        let header = guest_range(memory.len(), bank_size, pointer.bank, pointer.offset, 2)?;
        if memory.read(header.start) != ABI_VERSION {
            return Err(STATUS_VERSION);
        }
        if usize::from(memory.read(header.start + 1)) != length {
            return Err(STATUS_BAD_DESCRIPTOR);
        }
        guest_range(
            memory.len(),
            bank_size,
            pointer.bank,
            pointer.offset,
            length,
        )
    }

    fn execute_command<M: GuestMemory + ?Sized>(
        &mut self,
        command: u16,
        memory: &mut M,
        bank_size: u16,
        display_mode: u16,
        storage: Option<&mut crate::vm::Storage>,
    ) -> Result<Option<PresentFrame>, u16> {
        let length = match command {
            COMMAND_PRESENT | COMMAND_NEXT_TICK | COMMAND_GET_TIME | COMMAND_TTY_WRITE => 8,
            COMMAND_INPUT_READ => 10,
            COMMAND_HOST_MONOTONIC | COMMAND_HOST_REALTIME => 10,
            COMMAND_INPUT_CONFIGURE => 4,
            COMMAND_HOST_IDLE => 4,
            COMMAND_HOST_TERMINAL => 4,
            COMMAND_HOST_RANDOM | crate::crypto::COMMAND => 6,
            COMMAND_CAPABILITIES => 4,
            crate::float::COMMAND => crate::float::CELLS,
            COMMAND_INPUT_STATE => STATE_CELLS,
            _ => crate::network::descriptor_length(command)
                .or_else(|| crate::audio::descriptor_length(command))
                .or_else(|| crate::bulk::descriptor_length(command))
                .ok_or(STATUS_UNKNOWN_COMMAND)?,
        };
        if crate::audio::descriptor_length(command).is_some() && !self.audio.supported() {
            return Err(STATUS_UNSUPPORTED);
        }
        let range = Self::descriptor_range(self.descriptor, memory, bank_size, length)?;
        if crate::bulk::descriptor_length(command).is_some() {
            crate::bulk::execute(command, memory, range, bank_size, storage)?;
            self.status = STATUS_OK;
            return Ok(None);
        }
        if crate::audio::descriptor_length(command).is_some() {
            self.audio.execute(command, memory, range, bank_size)?;
            self.status = STATUS_OK;
            return Ok(None);
        }
        if crate::network::descriptor_length(command).is_some() {
            self.network.execute(command, memory, range, bank_size)?;
            self.status = STATUS_OK;
            return Ok(None);
        }
        let mut words = [0u16; STATE_CELLS];
        for (word, address) in words.iter_mut().zip(range.clone()) {
            *word = memory.read(address);
        }
        let descriptor = &mut words[..length];
        self.status = STATUS_OK;
        match command {
            crate::float::COMMAND => crate::float::execute(descriptor)?,
            COMMAND_TTY_WRITE => {
                crate::terminal::execute(memory, bank_size, range, descriptor)?;
                return Ok(None);
            }
            COMMAND_PRESENT => {
                if display_mode != DISPLAY_MODE_EXTENDED {
                    return Err(STATUS_DISPLAY_MODE);
                }
                let width = descriptor[2];
                let height = descriptor[3];
                let stride = descriptor[4];
                if width == 0 || height == 0 || stride < width || descriptor[7] != 0 {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                let span = usize::from(height - 1)
                    .checked_mul(usize::from(stride))
                    .and_then(|rows| rows.checked_add(usize::from(width)))
                    .ok_or(STATUS_BOUNDS)?;
                let buffer =
                    guest_range(memory.len(), bank_size, descriptor[5], descriptor[6], span)?;
                let size = usize::from(width)
                    .checked_mul(usize::from(height))
                    .ok_or(STATUS_BOUNDS)?;
                let mut pixels = Vec::new();
                pixels.try_reserve_exact(size).map_err(|_| STATUS_BOUNDS)?;
                for row in 0..usize::from(height) {
                    let start = buffer.start + row * usize::from(stride);
                    pixels.extend((start..start + usize::from(width)).map(|i| memory.read(i)));
                }
                return Ok(Some(PresentFrame {
                    width,
                    height,
                    pixels,
                }));
            }
            COMMAND_CAPABILITIES => {
                descriptor[2] = (CAPABILITIES & !(crate::audio::CAP_PCM | crate::audio::CAP_MUSIC))
                    | if self.audio.supported() {
                        crate::audio::CAP_PCM | crate::audio::CAP_MUSIC
                    } else {
                        0
                    };
                descriptor[2] |= crate::bulk::capabilities(memory, bank_size, storage.is_some());
                descriptor[3] = KEY_COUNT as u16;
            }
            COMMAND_NEXT_TICK => {
                self.pending_tick = Some(self.descriptor);
                self.status = STATUS_PENDING;
                return Ok(None);
            }
            COMMAND_GET_TIME => self.write_time(descriptor),
            COMMAND_HOST_MONOTONIC => {
                crate::host_clock::write_time(descriptor, self.host_clock.monotonic());
            }
            COMMAND_HOST_REALTIME => {
                crate::host_clock::write_time(
                    descriptor,
                    crate::host_clock::HostClock::realtime()?,
                );
            }
            COMMAND_HOST_IDLE => {
                if descriptor[2] > HOST_IDLE_MAX_MS || descriptor[3] != 0 {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                #[cfg(target_arch = "wasm32")]
                crate::web::idle(descriptor[2]);
                #[cfg(not(target_arch = "wasm32"))]
                if descriptor[2] == 0 {
                    std::thread::yield_now();
                } else {
                    std::thread::sleep(std::time::Duration::from_millis(u64::from(descriptor[2])));
                }
                // There are no output cells. Avoid dirtying RAM/COW pages just
                // to write the same descriptor back after every idle turn.
                return Ok(None);
            }
            crate::crypto::COMMAND => {
                crate::crypto::execute(memory, bank_size, range, descriptor)?;
                return Ok(None);
            }
            COMMAND_HOST_RANDOM => {
                crate::entropy::execute(memory, bank_size, range, descriptor, |output| {
                    #[cfg(not(target_arch = "wasm32"))]
                    { getrandom::fill(output).map_err(|_| ()) }
                    #[cfg(target_arch = "wasm32")]
                    { crate::web::random(output) }
                })?;
                return Ok(None);
            }
            COMMAND_HOST_TERMINAL => {
                if descriptor[2] != 0 || descriptor[3] != 0 {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                #[cfg(not(target_arch = "wasm32"))]
                let (columns, rows) = crossterm::terminal::size().map_err(|_| STATUS_UNSUPPORTED)?;
                #[cfg(target_arch = "wasm32")]
                let (columns, rows) = (80, 30);
                if rows == 0 || columns == 0 {
                    return Err(STATUS_UNSUPPORTED);
                }
                descriptor[2] = rows;
                descriptor[3] = columns;
            }
            COMMAND_INPUT_READ => {
                let capacity = usize::from(descriptor[2]);
                if capacity == 0 || capacity > MAX_BATCH || descriptor[9] != 0 {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                let buffer = guest_range(
                    memory.len(),
                    bank_size,
                    descriptor[3],
                    descriptor[4],
                    capacity * EVENT_CELLS,
                )?;
                if buffer.start < range.end && range.start < buffer.end {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                let count = capacity.min(self.input.events.len());
                for i in 0..count {
                    let event = self.input.events.pop_front().expect("count checked");
                    for (j, cell) in event.cells().into_iter().enumerate() {
                        memory.write(buffer.start + i * EVENT_CELLS + j, cell);
                    }
                }
                descriptor[5] = count as u16;
                descriptor[6] = u16::from(self.input.overflow);
                descriptor[7] = self.input.sequence as u16;
                descriptor[8] = (self.input.sequence >> 16) as u16;
            }
            COMMAND_INPUT_STATE => {
                descriptor.copy_from_slice(&self.input.snapshot_cells());
            }
            COMMAND_INPUT_CONFIGURE => {
                if descriptor[3] != 0 || !self.input.configure(descriptor[2]) {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
            }
            _ => unreachable!("command was validated above"),
        }
        for (address, &word) in range.zip(descriptor.iter()) {
            memory.write(address, word);
        }
        Ok(None)
    }

    fn write_time(&self, descriptor: &mut [u16]) {
        let millis = self.millis();
        descriptor[2] = self.tick as u16;
        descriptor[3] = (self.tick >> 16) as u16;
        descriptor[4] = millis as u16;
        descriptor[5] = (millis >> 16) as u16;
        descriptor[6] = self.tick_hz;
        descriptor[7] = 0;
    }

    /// Deliver live input at the current logical tick, including tick zero.
    /// Delivery neither advances time nor releases future replay events.
    pub fn accept_input(&mut self, events: impl IntoIterator<Item = KeyTransition>) {
        for event in events {
            self.input.apply(event.into(), self.tick);
        }
    }

    /// Hosts collect window input only for an input command that can execute.
    pub fn is_input_request(&self, address: usize, value: u16) -> bool {
        address == REGISTER_COMMAND
            && !self.has_pending_tick()
            && matches!(value, COMMAND_INPUT_READ | COMMAND_INPUT_STATE)
    }

    pub fn accept_events(&mut self, events: impl IntoIterator<Item = InputTransition>) {
        for event in events {
            self.input.apply(event, self.tick);
        }
    }

    /// Grant one pending tick, apply scripted/explicit tick-scheduled events,
    /// and atomically return the new clock value. Live window input is collected
    /// independently by INPUT_READ/INPUT_STATE, never by advancing the clock.
    /// Calling without a pending command has no effect.
    pub fn complete_tick(
        &mut self,
        memory: &mut [u16],
        bank_size: u16,
        tick_events: &[InputTransition],
    ) -> Result<(), String> {
        self.complete_tick_with_memory(memory, bank_size, tick_events)
    }

    pub fn complete_tick_with_memory<M: GuestMemory + ?Sized>(
        &mut self,
        memory: &mut M,
        bank_size: u16,
        tick_events: &[InputTransition],
    ) -> Result<(), String> {
        let Some(pointer) = self.pending_tick.take() else {
            return Ok(());
        };
        let range = Self::descriptor_range(pointer, memory, bank_size, 8).map_err(|status| {
            self.status = status;
            format!("NEXT_TICK descriptor became invalid (device status {status})")
        })?;
        self.tick = self.tick.checked_add(1).ok_or_else(|| {
            self.status = STATUS_BAD_DESCRIPTOR;
            "virtual tick counter exhausted".to_string()
        })?;
        while let Some(event) = self.script.events.get(self.script_position) {
            if event.tick > self.tick {
                break;
            }
            let transition = event.transition;
            self.script_position += 1;
            self.input.apply(transition, self.tick);
        }
        self.accept_events(tick_events.iter().copied());
        self.audio.advance_tick(self.tick_hz).map_err(|status| {
            self.status = status;
            format!(
                "PCM rendering failed: {}",
                self.audio.error().unwrap_or("audio timeline overflow")
            )
        })?;
        let mut descriptor = [0u16; 8];
        self.write_time(&mut descriptor);
        for offset in 2..8 {
            memory.write(range.start + offset, descriptor[offset]);
        }
        self.status = if self.input.overflow {
            STATUS_INPUT_OVERFLOW
        } else {
            STATUS_OK
        };
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn host_random_is_an_octet_service_independent_of_guest_time() {
        let mut state = DeviceState::default();
        let mut ram = vec![0xaaaa; 256];
        command(&mut state, &mut ram, COMMAND_CAPABILITIES, 4);
        assert_ne!(ram[42] & CAP_HOST_RANDOM, 0);
        ram[42..46].copy_from_slice(&[2, 0, 32, 0]);
        command(&mut state, &mut ram, COMMAND_HOST_RANDOM, 6);
        assert_eq!(state.status(), STATUS_OK);
        assert!(ram[128..160].iter().all(|&n| n <= 255));
        assert_eq!(ram[127], 0xaaaa);
        assert_eq!(ram[160], 0xaaaa);
        assert_eq!(state.tick(), 0);
        assert!(!state.has_pending_tick());
        let before = ram.clone();
        ram[45] = 1;
        let invalid = ram.clone();
        command(&mut state, &mut ram, COMMAND_HOST_RANDOM, 6);
        assert_eq!(state.status(), STATUS_BAD_DESCRIPTOR);
        assert_eq!(ram, invalid);
        assert_eq!(&ram[128..160], &before[128..160]);
    }

    #[test]
    fn host_terminal_size_validates_without_advancing_guest_time() {
        let mut state = DeviceState::default();
        let mut ram = vec![0u16; 256];
        command(&mut state, &mut ram, COMMAND_CAPABILITIES, 4);
        assert_ne!(ram[42] & CAP_HOST_TERMINAL, 0);
        for (version, length, payload, expected) in [
            (2, 4, 0, STATUS_VERSION),
            (1, 3, 0, STATUS_BAD_DESCRIPTOR),
            (1, 4, 1, STATUS_BAD_DESCRIPTOR),
        ] {
            ram[40..44].copy_from_slice(&[version, length, payload, 0]);
            let before = ram.clone();
            state.write_register(REGISTER_COMMAND, COMMAND_HOST_TERMINAL, &mut ram, 64, 4);
            assert_eq!(state.status(), expected);
            assert_eq!(ram, before);
        }
        ram[40..44].copy_from_slice(&[1, 4, 0, 0]);
        let before = ram.clone();
        state.write_register(REGISTER_COMMAND, COMMAND_HOST_TERMINAL, &mut ram, 64, 4);
        if state.status() == STATUS_OK {
            assert!(ram[42] > 0 && ram[43] > 0);
        } else {
            assert_eq!(state.status(), STATUS_UNSUPPORTED);
            assert_eq!(ram, before);
        }
        assert_eq!(state.tick(), 0);
        assert!(!state.has_pending_tick());
    }

    #[test]
    fn host_clocks_validate_descriptors_and_do_not_advance_logical_time() {
        use std::time::{Duration, SystemTime, UNIX_EPOCH};
        let mut state = DeviceState::default();
        let mut ram = vec![0u16; 256];
        command(&mut state, &mut ram, COMMAND_CAPABILITIES, 4);
        assert_ne!(ram[42] & CAP_HOST_CLOCK, 0);
        for request in [COMMAND_HOST_MONOTONIC, COMMAND_HOST_REALTIME] {
            for (version, length, expected) in
                [(2, 10, STATUS_VERSION), (1, 9, STATUS_BAD_DESCRIPTOR)]
            {
                ram[40..50].copy_from_slice(&[version, length, 7, 7, 7, 7, 7, 7, 7, 7]);
                let before = ram.clone();
                state.write_register(REGISTER_COMMAND, request, &mut ram, 64, 4);
                assert_eq!(state.status(), expected);
                assert_eq!(ram, before);
            }
            let before = SystemTime::now().duration_since(UNIX_EPOCH).unwrap();
            command(&mut state, &mut ram, request, 10);
            assert_eq!(state.status(), STATUS_OK);
            let seconds = (0..4).fold(0u64, |n, i| n | u64::from(ram[42 + i]) << (i * 16));
            let nanos = u32::from(ram[46]) | u32::from(ram[47]) << 16;
            let resolution = u32::from(ram[48]) | u32::from(ram[49]) << 16;
            assert!(nanos < 1_000_000_000);
            assert_eq!(nanos % 1_000_000, 0);
            assert_eq!(resolution, 1_000_000);
            if request == COMMAND_HOST_REALTIME {
                let after = SystemTime::now().duration_since(UNIX_EPOCH).unwrap();
                let sampled = Duration::new(seconds, nanos);
                assert!(sampled + Duration::from_millis(1) >= before);
                assert!(sampled <= after);
            }
            assert_eq!(state.tick(), 0);
            assert!(!state.has_pending_tick());
        }
        // Restore an old checkpoint which predates the host-clock extension.
        let mut old = serde_json::to_value(DeviceState::default().checkpoint()).unwrap();
        old.as_object_mut().unwrap().remove("host_clock");
        let mut restored =
            DeviceState::restore_checkpoint(serde_json::from_value(old).unwrap()).unwrap();
        command(&mut restored, &mut ram, COMMAND_HOST_MONOTONIC, 10);
        assert_eq!(restored.status(), STATUS_OK);
        restored.reset();
        command(&mut restored, &mut ram, COMMAND_HOST_MONOTONIC, 10);
        assert_eq!(&ram[42..46], &[0, 0, 0, 0]);
    }

    #[test]
    fn batch_bounds_overlap_and_capacity_fail_without_consuming_input() {
        let mut state = DeviceState::new(35).unwrap();
        let mut ram = vec![0u16; 256];
        state.accept_events([
            InputTransition::Motion {
                x: -70000,
                y: 80000,
            },
            InputTransition::Button {
                button: 0,
                down: true,
                x: 1,
                y: 2,
            },
        ]);
        for (capacity, bank, offset, reserved, expected) in [
            (0, 1, 0, 0, STATUS_BAD_DESCRIPTOR),
            (65, 1, 0, 0, STATUS_BAD_DESCRIPTOR),
            (1, 0, 31, 0, STATUS_BOUNDS),
            (1, 3, 60, 0, STATUS_BOUNDS),
            (1, 0, 40, 0, STATUS_BAD_DESCRIPTOR),
            (1, 1, 0, 1, STATUS_BAD_DESCRIPTOR),
        ] {
            ram[40..50].copy_from_slice(&[1, 10, capacity, bank, offset, 99, 99, 99, 99, reserved]);
            let before = ram.clone();
            state.write_register(REGISTER_DESCRIPTOR_OFFSET, 40, &mut ram, 64, 4);
            state.write_register(REGISTER_COMMAND, COMMAND_INPUT_READ, &mut ram, 64, 4);
            assert_eq!(state.status(), expected);
            assert_eq!(ram, before);
            assert_eq!(state.input.events.len(), 2);
        }
        // Two complete records crossing a bank boundary; count and sequence are outputs.
        ram[40..50].copy_from_slice(&[1, 10, 2, 1, 60, 0, 0, 0, 0, 0]);
        state.write_register(REGISTER_COMMAND, COMMAND_INPUT_READ, &mut ram, 64, 4);
        assert_eq!(state.status(), STATUS_OK);
        assert_eq!(&ram[45..49], &[2, 0, 2, 0]);
        assert_eq!(ram[124], 2);
        assert_eq!(&ram[144..148], &[3, 1, 0, 1]);
        assert!(state.input.events.is_empty());
        ram[40..44].copy_from_slice(&[1, 4, 2, 0]);
        state.write_register(REGISTER_COMMAND, COMMAND_INPUT_CONFIGURE, &mut ram, 64, 4);
        assert_eq!(state.status(), STATUS_BAD_DESCRIPTOR);
        assert_eq!(state.input.pointer_mode, 0);
        ram[42] = 1;
        state.write_register(REGISTER_COMMAND, COMMAND_INPUT_CONFIGURE, &mut ram, 64, 4);
        assert_eq!(state.status(), STATUS_OK);
        assert_eq!(state.input.pointer_mode, 1);
        assert!(!state.input.captured);
    }

    #[test]
    fn relative_scripts_preserve_order_and_reject_overflow() {
        let script = InputScript::from_json(r#"[{"tick":1,"key":"A","down":true},{"tick":1,"key":"A","down":false},{"tick":3,"key":"B","down":true}]"#).unwrap();
        let shifted = script.clone().relative_to(42).unwrap();
        assert_eq!(
            shifted
                .events
                .iter()
                .map(|event| event.tick)
                .collect::<Vec<_>>(),
            [43, 43, 45]
        );
        assert_eq!(shifted.events[1].transition, script.events[1].transition);
        assert!(script.relative_to(u64::MAX - 2).is_err());
        assert!(InputScript::default().relative_to(u64::MAX).is_ok());
    }

    #[test]
    fn checkpoint_replays_pending_tick_and_script_cursor_exactly() {
        let mut state = DeviceState::new(70).unwrap();
        let mut memory = vec![0; 256];
        state
            .set_script(
                InputScript::from_json(
                    r#"[{"tick":1,"key":"A","down":true},{"tick":2,"key":"A","down":false}]"#,
                )
                .unwrap(),
            )
            .unwrap();
        tick(
            &mut state,
            &mut memory,
            &[KeyTransition {
                key: 82,
                down: true,
            }],
        );
        command(&mut state, &mut memory, COMMAND_NEXT_TICK, 8);
        let bytes = serde_json::to_vec(&state.checkpoint()).unwrap();
        let mut restored =
            DeviceState::restore_checkpoint(serde_json::from_slice(&bytes).unwrap()).unwrap();
        let mut copy = memory.clone();
        state.complete_tick(&mut memory, 64, &[]).unwrap();
        restored.complete_tick(&mut copy, 64, &[]).unwrap();
        assert_eq!(memory, copy);
        assert_eq!(
            serde_json::to_value(state).unwrap(),
            serde_json::to_value(restored).unwrap()
        );
        let mut corrupt: DeviceState = serde_json::from_slice(&bytes).unwrap();
        corrupt.script_position = usize::MAX;
        assert!(DeviceState::restore_checkpoint(corrupt).is_err());
    }

    struct StridedMemory(Vec<u16>);
    impl GuestMemory for StridedMemory {
        fn len(&self) -> usize {
            self.0.len() / 8
        }
        fn read(&self, index: usize) -> u16 {
            self.0[index * 8 + 4]
        }
        fn write(&mut self, index: usize, value: u16) {
            self.0[index * 8 + 4] = value;
        }
    }

    #[test]
    fn strided_memory_matches_flat_commands_without_touching_other_lanes() {
        let mut flat = vec![0u16; 256];
        let mut strided = StridedMemory(vec![0xa55a; flat.len() * 8]);
        for index in 0..flat.len() {
            strided.write(index, 0);
        }
        let mut native = DeviceState::default();
        let mut bf = DeviceState::default();
        let script = InputScript::from_json(r#"[{"tick":1,"key":"UP","down":true}]"#).unwrap();
        native.set_script(script.clone()).unwrap();
        bf.set_script(script).unwrap();
        for (command, length) in [
            (COMMAND_CAPABILITIES, 4),
            (COMMAND_NEXT_TICK, 8),
            (COMMAND_GET_TIME, 8),
            (COMMAND_INPUT_READ, 10),
            (COMMAND_INPUT_STATE, STATE_CELLS as u16),
            (COMMAND_PRESENT, 8),
            (99, 8),
        ] {
            // The descriptor crosses bank 0->1; framebuffer crosses bank 1->2.
            flat[63..91].fill(0);
            flat[63] = ABI_VERSION;
            flat[64] = length;
            if command == COMMAND_INPUT_READ {
                flat[65..73].copy_from_slice(&[1, 2, 0, 0, 0, 0, 0, 0]);
            }
            if command == COMMAND_PRESENT {
                flat[65..71].copy_from_slice(&[3, 2, 5, 1, 62, 0]);
                flat[126..134].copy_from_slice(&[1, 2, 3, 99, 99, 4, 5, 6]);
            }
            for (index, &value) in flat.iter().enumerate() {
                strided.write(index, value);
            }
            native.write_register(REGISTER_DESCRIPTOR_OFFSET, 63, &mut flat, 64, 4);
            bf.write_register_with_memory(REGISTER_DESCRIPTOR_OFFSET, 63, &mut strided, 64, 4);
            let expected = native.write_register(REGISTER_COMMAND, command, &mut flat, 64, 4);
            let actual =
                bf.write_register_with_memory(REGISTER_COMMAND, command, &mut strided, 64, 4);
            assert_eq!(actual, expected);
            if command == COMMAND_NEXT_TICK {
                native.complete_tick(&mut flat, 64, &[]).unwrap();
                bf.complete_tick_with_memory(&mut strided, 64, &[]).unwrap();
            }
            assert_eq!(bf.status(), native.status());
            assert_eq!(bf.tick(), native.tick());
            assert_eq!(
                (0..flat.len()).map(|i| strided.read(i)).collect::<Vec<_>>(),
                flat
            );
            assert!(strided
                .0
                .iter()
                .enumerate()
                .all(|(i, &v)| i % 8 == 4 || v == 0xa55a));
        }
    }

    fn command(
        device: &mut DeviceState,
        memory: &mut [u16],
        command: u16,
        length: u16,
    ) -> Option<PresentFrame> {
        memory[40] = ABI_VERSION;
        memory[41] = length;
        if command == COMMAND_INPUT_READ {
            memory[42..50].copy_from_slice(&[1, 1, 32, 0, 0, 0, 0, 0]);
        }
        device.write_register(
            REGISTER_DESCRIPTOR_OFFSET,
            40,
            memory,
            64,
            DISPLAY_MODE_EXTENDED,
        );
        device.write_register(REGISTER_COMMAND, command, memory, 64, DISPLAY_MODE_EXTENDED)
    }

    fn tick(device: &mut DeviceState, memory: &mut [u16], events: &[KeyTransition]) {
        command(device, memory, COMMAND_NEXT_TICK, 8);
        assert_eq!(device.status(), STATUS_PENDING);
        device
            .complete_tick(
                memory,
                64,
                &events.iter().copied().map(Into::into).collect::<Vec<_>>(),
            )
            .unwrap();
    }

    #[test]
    fn ranges_cross_banks_but_never_address_mmio_or_wrap() {
        assert_eq!(guest_range(256, 64, 1, 60, 8), Ok(124..132));
        assert_eq!(guest_range(256, 64, 0, 31, 2), Err(STATUS_BOUNDS));
        assert_eq!(guest_range(256, 64, 1, 64, 1), Err(STATUS_BOUNDS));
        assert_eq!(guest_range(256, 64, 3, 60, 8), Err(STATUS_BOUNDS));
        assert_eq!(
            guest_range(usize::MAX, 64, u16::MAX, 63, 2),
            Err(STATUS_BOUNDS)
        );
        assert_eq!(guest_range(256, 0, 1, 0, 1), Err(STATUS_BOUNDS));
        assert_eq!(guest_range(256, 64, 1, 0, usize::MAX), Err(STATUS_BOUNDS));
    }

    #[test]
    fn present_snapshots_strided_bank_spanning_ram() {
        let mut device = DeviceState::default();
        let mut memory = vec![0; 256];
        memory[42..48].copy_from_slice(&[3, 2, 5, 1, 62, 0]);
        memory[126..134].copy_from_slice(&[1, 2, 3, 90, 91, 4, 5, 6]);
        let frame = command(&mut device, &mut memory, COMMAND_PRESENT, 8).unwrap();
        assert_eq!((frame.width, frame.height), (3, 2));
        assert_eq!(frame.pixels, [1, 2, 3, 4, 5, 6]);
        memory[126] = 99;
        assert_eq!(frame.pixels[0], 1);
        assert_eq!(device.tick(), 0);
    }

    #[test]
    fn descriptors_may_cross_a_bank_boundary() {
        let mut device = DeviceState::default();
        let mut memory = vec![0; 128];
        memory[63..67].copy_from_slice(&[1, 4, 0, 0]);
        device.write_register(REGISTER_DESCRIPTOR_OFFSET, 63, &mut memory, 64, 4);
        device.write_register(REGISTER_COMMAND, COMMAND_CAPABILITIES, &mut memory, 64, 4);
        assert_eq!(device.status(), STATUS_OK);
        assert_eq!(&memory[65..67], &[CAPABILITIES, 256]);
    }

    #[test]
    fn failed_present_does_not_change_guest_memory() {
        let mut device = DeviceState::default();
        let mut memory = vec![0; 256];
        command(&mut device, &mut memory, COMMAND_PRESENT, 8);
        assert_eq!(device.status(), STATUS_BAD_DESCRIPTOR);
        memory[42..48].copy_from_slice(&[3, 2, 3, 3, 62, 0]);
        let before = memory.clone();
        assert!(command(&mut device, &mut memory, COMMAND_PRESENT, 8).is_none());
        assert_eq!(device.status(), STATUS_BOUNDS);
        assert_eq!(memory, before);
        memory[42..48].copy_from_slice(&[3, 2, 3, 1, 0, 1]);
        assert!(command(&mut device, &mut memory, COMMAND_PRESENT, 8).is_none());
        assert_eq!(device.status(), STATUS_BAD_DESCRIPTOR);
        memory[47] = 0;
        device.write_register(REGISTER_COMMAND, COMMAND_PRESENT, &mut memory, 64, 3);
        assert_eq!(device.status(), STATUS_DISPLAY_MODE);
    }

    #[test]
    fn malformed_commands_have_specific_status_and_readonly_registers() {
        let mut device = DeviceState::default();
        let mut memory = vec![0; 128];
        command(&mut device, &mut memory, 99, 4);
        assert_eq!(device.status(), STATUS_UNKNOWN_COMMAND);
        command(&mut device, &mut memory, COMMAND_CAPABILITIES, 3);
        assert_eq!(device.status(), STATUS_BAD_DESCRIPTOR);
        memory[40] = 2;
        device.write_register(REGISTER_COMMAND, COMMAND_CAPABILITIES, &mut memory, 64, 4);
        assert_eq!(device.status(), STATUS_VERSION);
        device.write_register(REGISTER_STATUS, 0, &mut memory, 64, 4);
        device.write_register(REGISTER_VERSION, 5, &mut memory, 64, 4);
        assert_eq!(device.read_register(REGISTER_STATUS), Some(STATUS_VERSION));
        assert_eq!(device.read_register(REGISTER_VERSION), Some(1));
    }

    #[test]
    fn logical_clock_only_advances_on_completed_ticks_without_drift() {
        let mut device = DeviceState::default();
        let mut memory = vec![0; 128];
        for _ in 0..35 {
            let before = device.tick();
            command(&mut device, &mut memory, COMMAND_GET_TIME, 8);
            command(&mut device, &mut memory, COMMAND_GET_TIME, 8);
            assert_eq!(device.tick(), before);
            command(&mut device, &mut memory, COMMAND_NEXT_TICK, 8);
            assert_eq!(device.tick(), before);
            device.complete_tick(&mut memory, 64, &[]).unwrap();
            device.complete_tick(&mut memory, 64, &[]).unwrap();
            assert_eq!(device.tick(), before + 1);
        }
        assert_eq!(device.millis(), 1000);
        assert_eq!(&memory[42..48], &[35, 0, 1000, 0, 35, 0]);
        assert!(device.set_tick_hz(70).is_err());
        assert!(DeviceState::new(0).is_err());
    }

    #[test]
    fn clock_words_wrap_independently_without_arithmetic_overflow() {
        let mut device = DeviceState::new(1000).unwrap();
        let mut memory = vec![0; 128];
        device.tick = u64::from(u32::MAX);
        tick(&mut device, &mut memory, &[]);
        assert_eq!(device.tick(), 1u64 << 32);
        assert_eq!(device.millis(), 0);
        assert_eq!(&memory[42..46], &[0, 0, 0, 0]);
        device.tick = u64::MAX - 1;
        tick(&mut device, &mut memory, &[]);
        assert_eq!(device.millis(), u32::MAX);
    }

    #[test]
    fn pending_tick_remembers_its_descriptor_and_rejects_invalidated_memory() {
        let mut device = DeviceState::default();
        let mut memory = vec![0; 128];
        command(&mut device, &mut memory, COMMAND_NEXT_TICK, 8);
        device.write_register(REGISTER_DESCRIPTOR_OFFSET, 60, &mut memory, 64, 4);
        device.complete_tick(&mut memory, 64, &[]).unwrap();
        assert_eq!(memory[42], 1);
        assert_eq!(memory[62], 0);

        command(&mut device, &mut memory, COMMAND_NEXT_TICK, 8);
        memory[40] = 2;
        assert!(device.complete_tick(&mut memory, 64, &[]).is_err());
        assert_eq!(device.status(), STATUS_VERSION);
        assert_eq!(device.tick(), 1);
        assert!(!device.has_pending_tick());
    }

    #[test]
    fn script_order_is_preserved_and_duplicate_transitions_are_suppressed() {
        let mut device = DeviceState::default();
        let mut memory = vec![0; 128];
        let script = InputScript::from_json(
            r#"[
            {"tick":1,"key":"ENTER","down":true},
            {"tick":1,"key":"ENTER","down":true},
            {"tick":1,"key":"ENTER","down":false},
            {"tick":2,"key":4,"down":true}
        ]"#,
        )
        .unwrap();
        device.set_script(script).unwrap();
        tick(
            &mut device,
            &mut memory,
            &[KeyTransition {
                key: 82,
                down: true,
            }],
        );
        assert_eq!(
            device
                .input
                .events
                .iter()
                .map(|event| (event.code, event.flags & 1 != 0, event.tick))
                .collect::<Vec<_>>(),
            [(40, true, 1), (40, false, 1), (82, true, 1)]
        );
        assert!(!device.held(40));
        assert!(device.held(82));
        assert!(!device.held(4));
        tick(&mut device, &mut memory, &[]);
        assert!(device.held(4));
        command(&mut device, &mut memory, COMMAND_INPUT_READ, 10);
        assert_eq!(
            [
                memory[45],
                memory[98],
                memory[97],
                memory[102],
                memory[103],
                memory[46]
            ],
            [1, 40, 1, 1, 0, 0]
        );
        device.reset();
        assert_eq!(device.tick(), 0);
        assert!(device.input.events.is_empty());
        tick(&mut device, &mut memory, &[]);
        assert_eq!(device.input.events.len(), 2);
    }

    #[test]
    fn overflow_preserves_authoritative_held_state_and_lifetime_evidence() {
        let mut device = DeviceState::default();
        let mut memory = vec![0; 128];
        let mut events: Vec<_> = (0..256)
            .map(|key| KeyTransition { key, down: true })
            .collect();
        events.push(KeyTransition {
            key: 82,
            down: false,
        });
        tick(&mut device, &mut memory, &events);
        assert_eq!(device.input.events.len(), INPUT_QUEUE_CAPACITY);
        assert_eq!(device.status(), STATUS_INPUT_OVERFLOW);
        assert!(device.input_overflowed());
        assert!(device.overflow_seen());
        assert!(!device.held(82));
        command(
            &mut device,
            &mut memory,
            COMMAND_INPUT_STATE,
            STATE_CELLS as u16,
        );
        assert_eq!(memory[42], 1);
        assert_eq!(memory[52 + 82 / 16] & (1 << (82 % 16)), 0);
        assert!(!device.input_overflowed());
        assert!(device.overflow_seen());
        assert!(device.input.events.is_empty());
        assert_eq!(device.status(), STATUS_OK);

        tick(
            &mut device,
            &mut memory,
            &[KeyTransition {
                key: 82,
                down: true,
            }],
        );
        command(
            &mut device,
            &mut memory,
            COMMAND_INPUT_STATE,
            STATE_CELLS as u16,
        );
        assert_eq!(memory[42], 0);
        assert_eq!(device.input.events.len(), 1);
        command(&mut device, &mut memory, COMMAND_INPUT_READ, 10);
        assert_eq!(
            [
                memory[45],
                memory[98],
                memory[97],
                memory[102],
                memory[103],
                memory[46]
            ],
            [1, 82, 1, 2, 0, 0]
        );
    }

    #[test]
    fn invalid_scripts_and_names_fail_early() {
        for json in [
            r#"[{"tick":0,"key":"A","down":true}]"#,
            r#"[{"tick":2,"key":"A","down":true},{"tick":1,"key":"A","down":false}]"#,
            r#"[{"tick":1,"key":"DOES_NOT_EXIST","down":true}]"#,
            r#"[{"tick":1,"key":256,"down":true}]"#,
            r#"[{"tick":1,"key":"A","down":true,"typo":2}]"#,
        ] {
            assert!(InputScript::from_json(json).is_err(), "{json}");
        }
        assert_eq!(key_code("left_shift"), Some(225));
        assert_eq!(key_code("F24"), Some(115));
        assert_eq!(key_code("F25"), None);
        assert_eq!(key_code("0"), Some(39));
        assert_eq!(key_code("-"), Some(45));
        assert_eq!(key_code("num-pad-9"), Some(97));
    }
}
