//! MUS sequencing and GENMIDI instruments, synthesized on the host with Nuked OPL3.
//! Chip handles are ephemeral; checkpoints reconstruct them by replaying the
//! bounded score to the saved position, independently of output and wall time.
use nuked_opl3::Opl3Chip;
use serde::{Deserialize, Serialize};

pub const MAX_SCORE: usize = 131072;
pub const BANK_BYTES: usize = 8 + 175 * 36;
const MAX_TICKS: u32 = 140 * 30 * 60;

#[derive(Debug, Clone, PartialEq, Eq)]
struct Event {
    tick: u32,
    channel: usize,
    kind: Kind,
}
#[derive(Debug, Clone, PartialEq, Eq)]
enum Kind {
    Off(u8),
    On(u8, u8),
    Bend(u8),
    Control(u8, u8),
    End,
}
#[derive(Debug, Clone)]
struct Score {
    events: Vec<Event>,
    ticks: u32,
}
fn word(data: &[u8], at: usize) -> usize {
    usize::from(u16::from_le_bytes([data[at], data[at + 1]]))
}
fn byte(data: &[u8], at: &mut usize) -> Result<u8, String> {
    let b = *data.get(*at).ok_or("truncated MUS event")?;
    *at += 1;
    Ok(b)
}
fn seven(data: &[u8], at: &mut usize) -> Result<u8, String> {
    let b = byte(data, at)?;
    if b > 127 {
        Err("invalid MUS data byte".into())
    } else {
        Ok(b)
    }
}
fn parse(data: &[u8]) -> Result<Score, String> {
    if data.len() < 16 || data.len() > MAX_SCORE || &data[..4] != b"MUS\x1a" {
        return Err("invalid MUS header".into());
    }
    let (length, start, count) = (word(data, 4), word(data, 6), word(data, 12));
    if start < 16 + count * 2
        || start + length > data.len()
        || word(data, 8) > 15
        || word(data, 10) > 15
    {
        return Err("invalid MUS score range/channels".into());
    }
    let data = &data[start..start + length];
    let mut at = 0;
    let mut tick = 0u32;
    let mut velocity = [127u8; 16];
    let mut events = Vec::new();
    while at < data.len() && events.len() < 65536 {
        let tag = byte(data, &mut at)?;
        let channel = usize::from(tag & 15);
        let kind = match (tag >> 4) & 7 {
            0 => Kind::Off(seven(data, &mut at)?),
            1 => {
                let key = byte(data, &mut at)?;
                if key & 128 != 0 {
                    velocity[channel] = seven(data, &mut at)?;
                }
                Kind::On(key & 127, velocity[channel])
            }
            2 => Kind::Bend(byte(data, &mut at)?),
            3 => {
                let control = seven(data, &mut at)?;
                if !(10..=14).contains(&control) {
                    return Err("invalid MUS system event".into());
                }
                Kind::Control(control, 0)
            }
            4 => {
                let control = seven(data, &mut at)?;
                if control > 9 {
                    return Err("invalid MUS controller".into());
                }
                Kind::Control(control, seven(data, &mut at)?)
            }
            6 => {
                if tick == 0 {
                    return Err("MUS score has zero duration".into());
                }
                events.push(Event {
                    tick,
                    channel,
                    kind: Kind::End,
                });
                return Ok(Score {
                    events,
                    ticks: tick,
                });
            }
            _ => return Err("unsupported MUS event".into()),
        };
        events.push(Event {
            tick,
            channel,
            kind,
        });
        if tag & 128 != 0 {
            let mut delay = 0u32;
            let mut complete = false;
            for _ in 0..4 {
                let b = byte(data, &mut at)?;
                delay = (delay << 7) | u32::from(b & 127);
                if b & 128 == 0 {
                    complete = true;
                    break;
                }
            }
            if !complete {
                return Err("invalid MUS delay".into());
            }
            tick = tick
                .checked_add(delay)
                .filter(|t| *t <= MAX_TICKS)
                .ok_or("MUS score exceeds 30 minutes")?;
        }
    }
    Err("MUS score has no end event".into())
}
pub fn validate_bank(data: &[u8]) -> Result<(), String> {
    if data.len() != BANK_BYTES || &data[..8] != b"#OPL_II#" {
        return Err("invalid GENMIDI bank".into());
    }
    Ok(())
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Music {
    bank: Vec<u8>,
    score: Vec<u8>,
    playing: bool,
    paused: bool,
    looping: bool,
    position: u64,
    volume: u16,
    #[serde(skip)]
    renderer: Option<Renderer>,
}
impl std::fmt::Debug for Music {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Music")
            .field("playing", &self.playing)
            .field("position", &self.position)
            .finish_non_exhaustive()
    }
}
impl Default for Music {
    fn default() -> Self {
        Self {
            bank: Vec::new(),
            score: Vec::new(),
            playing: false,
            paused: false,
            looping: false,
            position: 0,
            volume: 127,
            renderer: None,
        }
    }
}
impl Clone for Music {
    fn clone(&self) -> Self {
        Self {
            bank: self.bank.clone(),
            score: self.score.clone(),
            playing: self.playing,
            paused: self.paused,
            looping: self.looping,
            position: self.position,
            volume: self.volume,
            renderer: None,
        }
    }
}
impl Music {
    pub fn load_bank(&mut self, data: Vec<u8>) -> Result<(), String> {
        validate_bank(&data)?;
        self.stop();
        self.bank = data;
        Ok(())
    }
    pub fn load_score(&mut self, data: Vec<u8>) -> Result<(), String> {
        parse(&data)?;
        self.stop();
        self.score = data;
        Ok(())
    }
    pub fn validate(&self, rate: u32) -> Result<(), String> {
        if !self.bank.is_empty() {
            validate_bank(&self.bank)?;
        }
        if self.volume > 127 {
            return Err("invalid music volume".into());
        }
        if !self.score.is_empty() {
            let score = parse(&self.score)?;
            if self.playing
                && (rate == 0
                    || self.bank.is_empty()
                    || self.position >= Self::duration(&score, rate))
            {
                return Err("invalid music playback checkpoint".into());
            }
        } else if self.playing || self.position != 0 {
            return Err("music checkpoint has no score".into());
        }
        if !self.playing && (self.paused || self.position != 0) {
            return Err("invalid stopped music checkpoint".into());
        }
        Ok(())
    }
    fn duration(score: &Score, rate: u32) -> u64 {
        (u64::from(score.ticks) * u64::from(rate)).div_ceil(140)
    }
    pub fn play(&mut self, looping: bool) -> bool {
        if self.bank.is_empty() || self.score.is_empty() {
            return false;
        }
        self.stop();
        self.playing = true;
        self.looping = looping;
        true
    }
    pub fn stop(&mut self) {
        self.playing = false;
        self.paused = false;
        self.position = 0;
        self.renderer = None;
    }
    pub fn pause(&mut self, paused: bool) {
        if self.playing {
            self.paused = paused;
        }
    }
    pub fn set_volume(&mut self, volume: u16) {
        self.volume = volume;
    }
    pub fn state(&self) -> (u16, u16, u64) {
        (
            u16::from(!self.bank.is_empty())
                | (u16::from(!self.score.is_empty()) << 1)
                | (u16::from(self.playing) << 2)
                | (u16::from(self.paused) << 3)
                | (u16::from(self.looping) << 4),
            self.volume,
            self.position,
        )
    }
    pub fn mix(&mut self, pcm: &mut [i16], channels: u16, rate: u32) {
        if !self.playing || self.paused {
            return;
        }
        if self.renderer.is_none() {
            let score = parse(&self.score).expect("validated MUS");
            let mut renderer = Renderer::new(score, rate);
            for _ in 0..self.position {
                renderer.next(&self.bank);
            }
            self.renderer = Some(renderer);
        }
        for frame in pcm.chunks_exact_mut(usize::from(channels)) {
            let renderer = self.renderer.as_mut().unwrap();
            let sample = renderer.next(&self.bank);
            for (channel, dest) in frame.iter_mut().enumerate() {
                let value = if channels == 1 {
                    (i32::from(sample[0]) + i32::from(sample[1])) / 2
                } else {
                    i32::from(sample[channel])
                };
                *dest = (i32::from(*dest) + value * i32::from(self.volume) / 127)
                    .clamp(-32768, 32767) as i16;
            }
            self.position += 1;
            if self.position >= Self::duration(&renderer.score, rate) {
                if !self.looping {
                    self.stop();
                    break;
                }
                self.position = 0;
                *renderer = Renderer::new(renderer.score.clone(), rate);
            }
        }
    }
}
#[derive(Clone, Copy)]
struct Channel {
    program: usize,
    volume: u8,
    expression: u8,
    pan: u8,
    bend: u8,
    sustain: bool,
}
impl Default for Channel {
    fn default() -> Self {
        Self {
            program: 0,
            volume: 100,
            expression: 127,
            pan: 64,
            bend: 128,
            sustain: false,
        }
    }
}
#[derive(Clone, Copy, Default)]
struct Voice {
    active: bool,
    released: bool,
    channel: usize,
    key: u8,
    velocity: u8,
    instrument: usize,
    layer: usize,
    age: u64,
    freq: u16,
}
struct Renderer {
    chip: Opl3Chip,
    score: Score,
    event: usize,
    frame: u64,
    rate: u32,
    channels: [Channel; 16],
    voices: [Voice; 18],
    age: u64,
}
impl Renderer {
    fn new(score: Score, rate: u32) -> Self {
        let mut chip = Opl3Chip::new(rate);
        chip.write_register(0x105, 1);
        chip.write_register(0x01, 0x20);
        chip.write_register(0x104, 0);
        Self {
            chip,
            score,
            event: 0,
            frame: 0,
            rate,
            channels: [Channel::default(); 16],
            voices: [Voice::default(); 18],
            age: 0,
        }
    }
    fn next(&mut self, bank: &[u8]) -> [i16; 2] {
        while let Some(event) = self.score.events.get(self.event) {
            if u64::from(event.tick) * u64::from(self.rate) / 140 > self.frame {
                break;
            }
            let event = event.clone();
            self.event += 1;
            self.event(bank, event);
        }
        let mut sample = [0; 2];
        self.chip
            .generate_resampled(&mut sample)
            .expect("stereo sample");
        self.frame += 1;
        sample
    }
    fn reg(voice: usize, base: u16) -> u16 {
        (if voice >= 9 { 0x100 } else { 0 }) + base + (voice % 9) as u16
    }
    fn op(voice: usize, carrier: bool) -> u16 {
        const SLOTS: [u16; 9] = [0, 1, 2, 8, 9, 10, 16, 17, 18];
        (if voice >= 9 { 0x100 } else { 0 }) + SLOTS[voice % 9] + if carrier { 3 } else { 0 }
    }
    fn off(&mut self, index: usize) {
        self.chip
            .write_register(Self::reg(index, 0xb0), (self.voices[index].freq >> 8) as u8);
        self.voices[index].active = false;
    }
    fn key_off(&mut self, channel: usize, key: Option<u8>, force: bool) {
        for i in 0..18 {
            let v = self.voices[i];
            if v.active && v.channel == channel && key.is_none_or(|k| v.key == k) {
                if self.channels[channel].sustain && !force {
                    self.voices[i].released = true;
                } else {
                    self.off(i);
                }
            }
        }
    }
    fn event(&mut self, bank: &[u8], event: Event) {
        let ch = event.channel;
        match event.kind {
            Kind::Off(key) => self.key_off(ch, Some(key), false),
            Kind::On(key, 0) => self.key_off(ch, Some(key), false),
            Kind::On(key, velocity) => {
                let instrument = if ch == 15 {
                    if !(35..=81).contains(&key) {
                        return;
                    }
                    128 + usize::from(key - 35)
                } else {
                    self.channels[ch].program
                };
                let flags = word(bank, 8 + instrument * 36);
                for layer in 0..if flags & 4 != 0 { 2 } else { 1 } {
                    let index = self
                        .voices
                        .iter()
                        .position(|v| !v.active)
                        .unwrap_or_else(|| {
                            self.voices
                                .iter()
                                .enumerate()
                                .min_by_key(|(_, v)| v.age)
                                .unwrap()
                                .0
                        });
                    self.off(index);
                    self.age += 1;
                    self.voices[index] = Voice {
                        active: true,
                        channel: ch,
                        key,
                        velocity,
                        instrument,
                        layer,
                        age: self.age,
                        ..Voice::default()
                    };
                    self.instrument(bank, index);
                    self.update(bank, index);
                }
            }
            Kind::Bend(bend) => {
                self.channels[ch].bend = bend;
                self.update_channel(bank, ch);
            }
            Kind::Control(control, value) => {
                match control {
                    0 => self.channels[ch].program = usize::from(value),
                    3 => self.channels[ch].volume = value,
                    4 => self.channels[ch].pan = value,
                    5 => self.channels[ch].expression = value,
                    8 => {
                        self.channels[ch].sustain = value >= 64;
                        if value < 64 {
                            for i in 0..18 {
                                let v = self.voices[i];
                                if v.active && v.channel == ch && v.released {
                                    self.off(i);
                                }
                            }
                        }
                    }
                    10 => self.key_off(ch, None, true),
                    11 => self.key_off(ch, None, false),
                    14 => {
                        let program = self.channels[ch].program;
                        self.channels[ch] = Channel {
                            program,
                            ..Channel::default()
                        };
                        for i in 0..18 {
                            let v = self.voices[i];
                            if v.active && v.channel == ch && v.released {
                                self.off(i);
                            }
                        }
                    }
                    _ => {}
                }
                self.update_channel(bank, ch);
            }
            Kind::End => {}
        }
    }
    fn instrument(&mut self, bank: &[u8], index: usize) {
        let v = self.voices[index];
        let at = 8 + v.instrument * 36 + 4 + v.layer * 16;
        for (carrier, offset) in [(false, 0), (true, 7)] {
            let op = Self::op(index, carrier);
            let data = &bank[at + offset..at + offset + 6];
            for (base, value) in [
                (0x20, data[0]),
                (0x60, data[1]),
                (0x80, data[2]),
                (0xe0, data[3] & 7),
            ] {
                self.chip.write_register(base + op, value);
            }
        }
    }
    fn update_channel(&mut self, bank: &[u8], channel: usize) {
        for i in 0..18 {
            if self.voices[i].active && self.voices[i].channel == channel {
                self.update(bank, i);
            }
        }
    }
    fn update(&mut self, bank: &[u8], index: usize) {
        let v = self.voices[index];
        let ch = self.channels[v.channel];
        let start = 8 + v.instrument * 36;
        let at = start + 4 + v.layer * 16;
        let gain = u32::from(v.velocity) * u32::from(ch.volume) * u32::from(ch.expression);
        for (carrier, offset) in [(false, 0), (true, 7)] {
            let level = bank[at + offset + 5] & 63;
            let level = if carrier || bank[at + 6] & 1 != 0 {
                63 - ((63 - u32::from(level)) * gain / (127 * 127 * 127)) as u8
            } else {
                level
            };
            self.chip.write_register(
                0x40 + Self::op(index, carrier),
                (bank[at + offset + 4] & 0xc0) | level,
            );
        }
        let pan = if ch.pan < 32 {
            0x10
        } else if ch.pan > 95 {
            0x20
        } else {
            0x30
        };
        self.chip
            .write_register(Self::reg(index, 0xc0), (bank[at + 6] & 15) | pan);
        let fixed = word(bank, start) & 1 != 0;
        let mut note = if fixed {
            f64::from(bank[start + 3])
        } else {
            // For non-fixed percussion patches, the key chooses an instrument;
            // its base pitch is middle C (the GENMIDI convention).
            f64::from(if v.channel == 15 { 60 } else { v.key })
                + f64::from(i16::from_le_bytes([bank[at + 14], bank[at + 15]]))
        };
        if v.layer != 0 {
            note += (f64::from(bank[start + 2]) - 128.0) / 64.0;
        }
        note += (f64::from(ch.bend) - 128.0) / 64.0;
        let frequency = 440.0 * 2.0f64.powf((note.clamp(0.0, 127.0) - 69.0) / 12.0);
        let mut fnum = frequency * 524288.0 / 49716.0;
        let mut block = 0;
        while fnum > 1023.0 && block < 7 {
            fnum *= 0.5;
            block += 1;
        }
        let freq = (fnum.round().min(1023.0) as u16) | (block << 10);
        self.voices[index].freq = freq;
        self.chip.write_register(Self::reg(index, 0xa0), freq as u8);
        self.chip
            .write_register(Self::reg(index, 0xb0), (freq >> 8) as u8 | 0x20);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn bank() -> Vec<u8> {
        let mut bank = vec![0; BANK_BYTES];
        bank[..8].copy_from_slice(b"#OPL_II#");
        for instrument in 0..175 {
            let at = 8 + instrument * 36 + 4;
            // Simple two-operator patch with a fast attack and sustained carrier.
            for op in [at, at + 7] {
                bank[op] = 0x21;
                bank[op + 1] = 0xf2;
                bank[op + 2] = 0x24;
            }
            bank[at + 5] = 32;
        }
        bank
    }
    fn score(events: &[u8]) -> Vec<u8> {
        let mut bytes = b"MUS\x1a".to_vec();
        bytes.extend((events.len() as u16).to_le_bytes());
        bytes.extend(16u16.to_le_bytes());
        bytes.extend([1, 0, 0, 0, 0, 0, 0, 0]);
        bytes.extend(events);
        bytes
    }
    fn player() -> Music {
        let mut music = Music::default();
        music.load_bank(bank()).unwrap();
        // Note 60 on, 140 ticks, off, another 140 ticks, end.
        music
            .load_score(score(&[
                0x90, 0xbc, 127, 0x81, 12, 0x80, 60, 0x81, 12, 0x60,
            ]))
            .unwrap();
        assert!(music.play(false));
        music
    }
    #[test]
    fn decoder_rejects_truncation_bad_controllers_and_unbounded_delays() {
        let valid = score(&[0x90, 60, 140, 0x60]);
        for end in 0..valid.len() {
            assert!(parse(&valid[..end]).is_err());
        }
        for events in [
            &[0x60][..],
            &[0x50],
            &[0x30, 9],
            &[0x40, 10, 0],
            &[0x90, 60, 0xff, 0xff, 0xff, 0xff, 0x60],
        ] {
            assert!(parse(&score(events)).is_err());
        }
        assert!(validate_bank(&vec![0; BANK_BYTES]).is_err());
    }
    #[test]
    fn exact_timing_pause_volume_end_loop_and_checkpoint_reconstruction() {
        let mut music = player();
        let mut first = vec![0; 22050 * 2];
        music.mix(&mut first, 2, 22050);
        assert!(first.iter().any(|s| *s != 0));
        assert_eq!(music.position, 22050);
        music.pause(true);
        let mut silence = vec![0; 800];
        music.mix(&mut silence, 2, 22050);
        assert!(silence.iter().all(|s| *s == 0));
        assert_eq!(music.position, 22050);
        music.pause(false);
        let mut restored: Music =
            serde_json::from_slice(&serde_json::to_vec(&music).unwrap()).unwrap();
        restored.validate(22050).unwrap();
        let mut a = vec![0; 4096];
        let mut b = a.clone();
        music.mix(&mut a, 2, 22050);
        restored.mix(&mut b, 2, 22050);
        assert_eq!(a, b);
        music.mix(&mut vec![0; 22050 * 2], 2, 22050);
        assert!(!music.playing);
        assert_eq!(music.position, 0);
        assert!(music.play(true));
        let mut two_loops = vec![0; 44100 * 2 * 2];
        music.mix(&mut two_loops, 2, 22050);
        assert_eq!(&two_loops[..88200], &two_loops[88200..]);
        assert_eq!(music.position, 0);
        assert!(music.playing);
        music.set_volume(0);
        let mut muted = vec![0; 400];
        music.mix(&mut muted, 2, 22050);
        assert!(muted.iter().all(|s| *s == 0));
        assert_eq!(music.position, 200);
    }
    #[test]
    fn note_release_sustain_percussion_and_dual_voice_are_handled() {
        let score = parse(&score(&[0x90, 60, 1, 0x60])).unwrap();
        let mut renderer = Renderer::new(score, 22050);
        let mut bank = bank();
        bank[8] = 4; // Two voices for instrument zero.
        renderer.event(
            &bank,
            Event {
                tick: 0,
                channel: 0,
                kind: Kind::On(60, 127),
            },
        );
        assert_eq!(renderer.voices.iter().filter(|v| v.active).count(), 2);
        renderer.event(
            &bank,
            Event {
                tick: 0,
                channel: 0,
                kind: Kind::Control(8, 127),
            },
        );
        renderer.event(
            &bank,
            Event {
                tick: 0,
                channel: 0,
                kind: Kind::Off(60),
            },
        );
        assert_eq!(
            renderer
                .voices
                .iter()
                .filter(|v| v.active && v.released)
                .count(),
            2
        );
        renderer.event(
            &bank,
            Event {
                tick: 0,
                channel: 0,
                kind: Kind::Control(8, 0),
            },
        );
        assert_eq!(renderer.voices.iter().filter(|v| v.active).count(), 0);
        renderer.event(
            &bank,
            Event {
                tick: 0,
                channel: 15,
                kind: Kind::On(35, 127),
            },
        );
        assert_eq!(renderer.voices[0].instrument, 128);
        let percussion_freq = renderer.voices[0].freq;
        renderer.event(
            &bank,
            Event {
                tick: 0,
                channel: 1,
                kind: Kind::On(60, 127),
            },
        );
        assert_eq!(renderer.voices[1].freq, percussion_freq);
        renderer.event(
            &bank,
            Event {
                tick: 0,
                channel: 15,
                kind: Kind::Control(10, 0),
            },
        );
        assert!(!renderer.voices[0].active);
    }
}
