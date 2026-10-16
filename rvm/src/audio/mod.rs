//! Bounded PCM device. Logical rendering is independent of the speaker clock.
mod music;
mod output;
#[cfg(test)]
mod tests;
use crate::devices::{
    guest_range, GuestMemory, STATUS_BAD_DESCRIPTOR, STATUS_BAD_STATE, STATUS_IO,
    STATUS_UNSUPPORTED, STATUS_WOULD_BLOCK,
};
use serde::{Deserialize, Serialize};
use std::{collections::VecDeque, ops::Range, path::Path};

#[derive(Debug, Clone, Copy, PartialEq, Eq, clap::ValueEnum)]
pub enum AudioMode {
    Auto,
    Off,
}
pub const CAP_PCM: u16 = 0x0080;
pub const CAP_MUSIC: u16 = 0x0100;
pub const CONFIGURE: u16 = 0x0200;
pub const SUBMIT: u16 = 0x0201;
pub const STATE: u16 = 0x0202;
pub const CONTROL: u16 = 0x0203;
pub const RENDER: u16 = 0x0204;
pub const MUSIC_BANK: u16 = 0x0210;
pub const MUSIC_SCORE: u16 = 0x0211;
pub const MUSIC_CONTROL: u16 = 0x0212;
pub const MUSIC_STATE: u16 = 0x0213;
pub const MUSIC_PLAY: u16 = 1;
pub const MUSIC_STOP: u16 = 2;
pub const MUSIC_PAUSE: u16 = 3;
pub const MUSIC_RESUME: u16 = 4;
pub const MUSIC_VOLUME: u16 = 5;
pub const START: u16 = 1;
pub const PAUSE: u16 = 2;
pub const STOP: u16 = 3;
pub const RESET: u16 = 4;
pub const GAIN: u16 = 5;
pub const MAX_FRAMES: usize = 192000;

pub fn descriptor_length(command: u16) -> Option<usize> {
    match command {
        CONFIGURE => Some(8),
        SUBMIT => Some(10),
        STATE => Some(16),
        CONTROL => Some(4),
        RENDER => Some(6),
        MUSIC_BANK | MUSIC_SCORE => Some(8),
        MUSIC_CONTROL => Some(4),
        MUSIC_STATE => Some(10),
        _ => None,
    }
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct PcmState {
    supported: bool,
    rate: u32,
    channels: u16,
    capacity: usize,
    running: bool,
    queue: VecDeque<i16>,
    position: u64,
    underruns: u32,
    gain: u16,
    tick_fraction: u64,
}
impl Default for PcmState {
    fn default() -> Self {
        Self {
            supported: true,
            rate: 0,
            channels: 0,
            capacity: 0,
            running: false,
            queue: VecDeque::new(),
            position: 0,
            underruns: 0,
            gain: u16::MAX,
            tick_fraction: 0,
        }
    }
}
#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct AudioDevice {
    state: PcmState,
    #[serde(default)]
    music: music::Music,
    #[serde(skip)]
    host: output::Output,
    #[serde(skip)]
    error: Option<String>,
}
impl Clone for AudioDevice {
    fn clone(&self) -> Self {
        Self {
            state: self.state.clone(),
            music: self.music.clone(),
            ..Self::default()
        }
    }
}
fn pair(words: &[u16], at: usize) -> u32 {
    u32::from(words[at]) | (u32::from(words[at + 1]) << 16)
}
fn put(words: &mut [u16], at: usize, value: u64, count: usize) {
    for n in 0..count {
        words[at + n] = (value >> (16 * n)) as u16;
    }
}
impl AudioDevice {
    pub fn supported(&self) -> bool {
        self.state.supported
    }
    pub fn disable(&mut self) {
        self.music = music::Music::default();
        self.state = PcmState {
            supported: false,
            ..PcmState::default()
        };
    }
    pub fn set_output(&mut self, live: bool, wav: Option<&Path>) -> Result<(), String> {
        self.host = output::Output::new(live, wav)?;
        Ok(())
    }
    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }
    pub fn finish(&mut self) -> Result<(), String> {
        self.host.finish()
    }
    pub fn validate(&self, tick_hz: u16) -> Result<(), String> {
        self.music.validate(self.state.rate)?;
        let s = &self.state;
        if s.rate == 0 {
            if s.channels != 0
                || s.capacity != 0
                || s.running
                || !s.queue.is_empty()
                || s.position != 0
                || s.underruns != 0
                || s.tick_fraction != 0
            {
                return Err("invalid unconfigured PCM checkpoint".into());
            }
        } else if !s.supported
            || !(8000..=96000).contains(&s.rate)
            || !matches!(s.channels, 1 | 2)
            || s.capacity == 0
            || s.capacity > MAX_FRAMES
            || s.capacity > s.rate as usize * 2
            || s.queue.len() > s.capacity * usize::from(s.channels)
            || s.queue.len() % usize::from(s.channels) != 0
            || s.tick_fraction >= u64::from(tick_hz)
        {
            return Err("invalid PCM checkpoint".into());
        }
        Ok(())
    }
    pub fn reset(&mut self) {
        self.music = music::Music::default();
        let supported = self.supported();
        self.host.clear_playback();
        self.state = PcmState {
            supported,
            ..PcmState::default()
        };
        self.error = None;
    }
    fn output_error(&mut self, error: String) -> u16 {
        self.error = Some(error);
        STATUS_IO
    }
    pub fn execute<M: GuestMemory + ?Sized>(
        &mut self,
        command: u16,
        memory: &mut M,
        range: Range<usize>,
        bank_size: u16,
    ) -> Result<(), u16> {
        if !self.supported() {
            return Err(STATUS_UNSUPPORTED);
        }
        let mut w: Vec<u16> = range.clone().map(|i| memory.read(i)).collect();
        match command {
            CONFIGURE => {
                let (rate, channels, capacity) = (pair(&w, 2), w[4], pair(&w, 5) as usize);
                if !(8000..=96000).contains(&rate)
                    || !matches!(channels, 1 | 2)
                    || capacity == 0
                    || capacity > MAX_FRAMES
                    || capacity > rate as usize * 2
                    || w[7] != 0
                {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                if self.state.running || !self.state.queue.is_empty() {
                    return Err(STATUS_BAD_STATE);
                }
                self.host
                    .configure(rate, channels)
                    .map_err(|e| self.output_error(e))?;
                self.state = PcmState {
                    rate,
                    channels,
                    capacity,
                    ..PcmState::default()
                };
                self.error = None;
                self.music = music::Music::default();
            }
            SUBMIT => {
                if self.state.rate == 0 {
                    return Err(STATUS_BAD_STATE);
                }
                let frames = pair(&w, 4) as usize;
                if frames == 0 || frames > self.state.capacity || w[6] != 0 || w[9] != 0 {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                let count = frames * usize::from(self.state.channels);
                let buffer = guest_range(memory.len(), bank_size, w[2], w[3], count)?;
                if buffer.start < range.end && range.start < buffer.end {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                if self.state.queue.len() + count
                    > self.state.capacity * usize::from(self.state.channels)
                {
                    return Err(STATUS_WOULD_BLOCK);
                }
                self.state
                    .queue
                    .extend(buffer.map(|i| memory.read(i) as i16));
                put(&mut w, 7, frames as u64, 2);
            }
            STATE => {
                let s = &self.state;
                w[2] = u16::from(s.rate != 0)
                    | (u16::from(s.running) << 1)
                    | (u16::from(s.underruns != 0) << 2)
                    | (u16::from(self.error.is_some()) << 3);
                w[3] = s.channels;
                put(&mut w, 4, u64::from(s.rate), 2);
                put(&mut w, 6, s.capacity as u64, 2);
                put(
                    &mut w,
                    8,
                    (s.queue.len() / usize::from(s.channels.max(1))) as u64,
                    2,
                );
                put(&mut w, 10, s.position, 4);
                put(&mut w, 14, u64::from(s.underruns), 2);
            }
            CONTROL => {
                if w[2] != GAIN && w[3] != 0 {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                if w[2] != RESET && self.state.rate == 0 {
                    return Err(STATUS_BAD_STATE);
                }
                match w[2] {
                    START => self.state.running = true,
                    PAUSE => {
                        self.state.running = false;
                        self.host.clear_playback();
                    }
                    STOP => {
                        self.state.running = false;
                        self.state.queue.clear();
                        self.state.tick_fraction = 0;
                        self.host.clear_playback();
                    }
                    RESET => self.reset(),
                    GAIN => self.state.gain = w[3],
                    _ => return Err(STATUS_BAD_DESCRIPTOR),
                }
            }
            RENDER => {
                let frames = pair(&w, 2) as usize;
                if frames == 0 || frames > MAX_FRAMES || w[4] != 0 || w[5] != 0 {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                if !self.state.running {
                    return Err(STATUS_BAD_STATE);
                }
                self.render(frames)?;
            }
            MUSIC_BANK | MUSIC_SCORE => {
                if self.state.rate == 0 {
                    return Err(STATUS_BAD_STATE);
                }
                let count = pair(&w, 4) as usize;
                if count == 0 || count > music::MAX_SCORE || w[6] != 0 || w[7] != 0 {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                let buffer = guest_range(memory.len(), bank_size, w[2], w[3], count)?;
                if buffer.start < range.end && range.start < buffer.end {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                let bytes: Vec<u8> = buffer
                    .map(|i| u8::try_from(memory.read(i)))
                    .collect::<Result<_, _>>()
                    .map_err(|_| STATUS_BAD_DESCRIPTOR)?;
                let result = if command == MUSIC_BANK {
                    self.music.load_bank(bytes)
                } else {
                    self.music.load_score(bytes)
                };
                result.map_err(|_| STATUS_BAD_DESCRIPTOR)?;
            }
            MUSIC_CONTROL => {
                if self.state.rate == 0 {
                    return Err(STATUS_BAD_STATE);
                }
                match (w[2], w[3]) {
                    (MUSIC_PLAY, looping @ 0..=1) => {
                        if !self.music.play(looping != 0) {
                            return Err(STATUS_BAD_STATE);
                        }
                    }
                    (MUSIC_STOP, 0) => self.music.stop(),
                    (MUSIC_PAUSE, 0) => self.music.pause(true),
                    (MUSIC_RESUME, 0) => self.music.pause(false),
                    (MUSIC_VOLUME, volume @ 0..=127) => self.music.set_volume(volume),
                    _ => return Err(STATUS_BAD_DESCRIPTOR),
                }
            }
            MUSIC_STATE => {
                let (flags, volume, position) = self.music.state();
                w[2] = flags;
                w[3] = volume;
                put(&mut w, 4, position, 4);
                w[8] = 0;
                w[9] = 0;
            }
            _ => return Err(STATUS_BAD_DESCRIPTOR),
        }
        for (i, word) in range.zip(w) {
            memory.write(i, word);
        }
        Ok(())
    }
    pub fn advance_tick(&mut self, hz: u16) -> Result<(), u16> {
        if !self.state.supported || !self.state.running {
            return Ok(());
        }
        let total = self.state.tick_fraction + u64::from(self.state.rate);
        let frames = (total / u64::from(hz)) as usize;
        self.state.tick_fraction = total % u64::from(hz);
        self.render(frames)
    }
    fn render(&mut self, frames: usize) -> Result<(), u16> {
        let s = &mut self.state;
        let position = s
            .position
            .checked_add(frames as u64)
            .ok_or(STATUS_BAD_STATE)?;
        let available = s.queue.len() / usize::from(s.channels);
        let missing = frames.saturating_sub(available);
        let mut pcm = vec![0i16; frames * usize::from(s.channels)];
        for sample in pcm
            .iter_mut()
            .take(frames.min(available) * usize::from(s.channels))
        {
            let value = s.queue.pop_front().expect("available checked");
            *sample = value;
        }
        self.music.mix(&mut pcm, s.channels, s.rate);
        for sample in &mut pcm {
            *sample = (i32::from(*sample) * i32::from(s.gain) / 65535) as i16;
        }
        s.position = position;
        s.underruns = s.underruns.saturating_add(missing as u32);
        self.host
            .write(s.rate, s.channels, &pcm)
            .map_err(|e| self.output_error(e))
    }
}
