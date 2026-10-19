//! Ephemeral speaker/WAV sinks. Guest state and deterministic PCM never use wall time.
use cpal::{
    traits::{DeviceTrait, HostTrait, StreamTrait},
    FromSample,
};
use std::{
    collections::VecDeque,
    fs::File,
    io::{Seek, SeekFrom, Write},
    path::Path,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

#[derive(Default)]
pub(super) struct Output {
    live: bool,
    attempted: bool,
    speaker: Option<cpal::Stream>,
    playback: Option<Arc<Mutex<Playback>>>,
    wav: Option<Wav>,
}
impl std::fmt::Debug for Output {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Output")
            .field("live", &self.live)
            .finish_non_exhaustive()
    }
}
impl Output {
    pub fn new(live: bool, path: Option<&Path>) -> Result<Self, String> {
        let wav = path.map(Wav::new).transpose()?;
        Ok(Self {
            live,
            wav,
            ..Self::default()
        })
    }
    pub fn configure(&mut self, rate: u32, channels: u16) -> Result<(), String> {
        if let Some(wav) = &mut self.wav {
            wav.configure(rate, channels)?;
        }
        self.speaker = None;
        self.playback = None;
        self.attempted = false;
        // Opening a physical device can block. Do it while the guest is
        // configuring a paused stream, before its real-time clock starts.
        self.ensure_speaker(rate);
        Ok(())
    }
    pub fn clear_playback(&mut self) {
        if let Some(p) = &self.playback {
            let mut p = p.lock().unwrap();
            p.frames.clear();
            p.phase = 0;
            p.drain_until = None;
            p.started = false;
            p.empty_frames = 0;
        }
    }
    pub fn write(&mut self, rate: u32, channels: u16, pcm: &[i16]) -> Result<(), String> {
        if let Some(wav) = &mut self.wav {
            wav.write(rate, channels, pcm)?;
        }
        self.ensure_speaker(rate);
        if let Some(p) = &self.playback {
            let mut p = p.lock().unwrap();
            p.resume();
            for frame in pcm.chunks_exact(usize::from(channels)) {
                // Bound host latency independently of the guest's logical queue.
                if p.frames.len() >= rate as usize / 4 {
                    p.frames.pop_front();
                }
                p.frames.push_back([
                    f32::from(frame[0]) / 32768.0,
                    f32::from(frame[usize::from(channels) - 1]) / 32768.0,
                ]);
            }
        }
        Ok(())
    }
    fn ensure_speaker(&mut self, rate: u32) {
        if self.live && !self.attempted {
            self.attempted = true;
            match speaker(rate) {
                Ok((stream, queue)) => {
                    self.speaker = Some(stream);
                    self.playback = Some(queue);
                }
                Err(e) => eprintln!("Audio output unavailable; continuing muted: {e}"),
            }
        }
    }
    pub fn finish(&mut self) -> Result<(), String> {
        if let Some(wav) = &mut self.wav {
            wav.file.flush().map_err(|e| e.to_string())?;
        }
        Ok(())
    }
    pub fn live(&self) -> bool {
        self.speaker.is_some()
    }
    // Count gaps only once subsequent PCM arrives. Initial silence, a normal
    // final drain, and explicit pause/stop are not playback underruns.
    pub fn statistics(&self) -> (u64, u64, u64) {
        self.playback.as_ref().map_or((0, 0, 0), |p| {
            let p = p.lock().unwrap();
            (
                p.gaps,
                p.missing_frames,
                p.max_gap_frames.saturating_mul(1000) / u64::from(p.output_rate),
            )
        })
    }
    pub fn pending_frames(&self, rate: u32) -> u32 {
        let Some(playback) = &self.playback else {
            return 0;
        };
        let queue = playback.lock().unwrap();
        let hardware = queue
            .drain_until
            .and_then(|end| end.checked_duration_since(Instant::now()))
            .map_or(0, |left| {
                (left.as_secs_f64() * f64::from(rate)).ceil() as u32
            });
        (queue.frames.len() as u32).saturating_add(hardware)
    }
}
struct Playback {
    frames: VecDeque<[f32; 2]>,
    phase: u64,
    source_rate: u32,
    output_rate: u32,
    drain_until: Option<Instant>,
    started: bool,
    empty_frames: u64,
    gaps: u64,
    missing_frames: u64,
    max_gap_frames: u64,
    preroll_frames: usize,
}
impl Playback {
    fn resume(&mut self) {
        if self.empty_frames != 0 {
            self.gaps = self.gaps.saturating_add(1);
            self.missing_frames = self.missing_frames.saturating_add(self.empty_frames);
            self.max_gap_frames = self.max_gap_frames.max(self.empty_frames);
            self.empty_frames = 0;
        }
        // The Unix renderer submits elapsed wall time in small bursts. Keep
        // 40 ms ahead of the speaker so a late checkpoint is not audible.
        // This sink-only lead never changes deterministic PCM/WAV captures.
        if !self.started && self.frames.is_empty() {
            self.frames.resize(self.preroll_frames, [0.0; 2]);
        }
    }
    fn next(&mut self) -> [f32; 2] {
        let Some(a) = self.frames.front().copied() else {
            self.phase = 0;
            if self.started {
                self.empty_frames = self.empty_frames.saturating_add(1);
            }
            return [0.0; 2];
        };
        self.started = true;
        let b = self.frames.get(1).copied().unwrap_or(a);
        let f = self.phase as f32 / self.output_rate as f32;
        let result = [a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f];
        self.phase += u64::from(self.source_rate);
        while self.phase >= u64::from(self.output_rate) {
            self.phase -= u64::from(self.output_rate);
            self.frames.pop_front();
        }
        result
    }
}
fn speaker(rate: u32) -> Result<(cpal::Stream, Arc<Mutex<Playback>>), String> {
    let device = cpal::default_host()
        .default_output_device()
        .ok_or("no default output device")?;
    let supported = device.default_output_config().map_err(|e| e.to_string())?;
    let config: cpal::StreamConfig = supported.clone().into();
    let queue = Arc::new(Mutex::new(Playback {
        frames: VecDeque::new(),
        phase: 0,
        source_rate: rate,
        output_rate: config.sample_rate,
        drain_until: None,
        started: false,
        empty_frames: 0,
        gaps: 0,
        missing_frames: 0,
        max_gap_frames: 0,
        preroll_frames: (rate / 25) as usize,
    }));
    let stream = match supported.sample_format() {
        cpal::SampleFormat::F32 => build::<f32>(&device, config, queue.clone()),
        cpal::SampleFormat::F64 => build::<f64>(&device, config, queue.clone()),
        cpal::SampleFormat::I16 => build::<i16>(&device, config, queue.clone()),
        cpal::SampleFormat::U16 => build::<u16>(&device, config, queue.clone()),
        cpal::SampleFormat::I32 => build::<i32>(&device, config, queue.clone()),
        cpal::SampleFormat::U32 => build::<u32>(&device, config, queue.clone()),
        format => return Err(format!("unsupported output format {format}")),
    }
    .map_err(|e| e.to_string())?;
    stream.play().map_err(|e| e.to_string())?;
    Ok((stream, queue))
}
fn build<T: cpal::SizedSample + FromSample<f32>>(
    device: &cpal::Device,
    config: cpal::StreamConfig,
    queue: Arc<Mutex<Playback>>,
) -> Result<cpal::Stream, cpal::Error> {
    let channels = usize::from(config.channels);
    let output_rate = config.sample_rate;
    device.build_output_stream(
        config,
        move |data: &mut [T], info: &cpal::OutputCallbackInfo| {
            let mut queue = queue.lock().unwrap();
            let mut last = None;
            for (index, frame) in data.chunks_exact_mut(channels).enumerate() {
                if !queue.frames.is_empty() {
                    last = Some(index + 1);
                }
                let pair = queue.next();
                if channels == 1 {
                    frame[0] = T::from_sample((pair[0] + pair[1]) * 0.5);
                } else {
                    for (n, sample) in frame.iter_mut().enumerate() {
                        *sample = if n < 2 {
                            T::from_sample(pair[n])
                        } else {
                            T::EQUILIBRIUM
                        };
                    }
                }
            }
            if let Some(frames) = last {
                let timestamp = info.timestamp();
                let delay = timestamp.playback.duration_since(timestamp.callback)
                    + Duration::from_secs_f64(frames as f64 / f64::from(output_rate));
                queue.drain_until = Instant::now().checked_add(delay);
            }
        },
        |error| eprintln!("Audio stream error: {error}"),
        None,
    )
}
struct Wav {
    file: File,
    rate: u32,
    channels: u16,
    bytes: u32,
}
impl Wav {
    fn new(path: &Path) -> Result<Self, String> {
        if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
            std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        }
        let mut file = File::create(path)
            .map_err(|e| format!("Creating audio capture {}: {e}", path.display()))?;
        file.write_all(&[0; 44]).map_err(|e| e.to_string())?;
        let mut wav = Self {
            file,
            rate: 22050,
            channels: 2,
            bytes: 0,
        };
        wav.header()?;
        Ok(wav)
    }
    fn configure(&mut self, rate: u32, channels: u16) -> Result<(), String> {
        if self.bytes != 0 && (self.rate != rate || self.channels != channels) {
            return Err("cannot change PCM format inside one WAV capture".into());
        }
        self.rate = rate;
        self.channels = channels;
        self.header()
    }
    fn header(&mut self) -> Result<(), String> {
        let mut h = Vec::with_capacity(44);
        h.extend(b"RIFF");
        h.extend((36 + self.bytes).to_le_bytes());
        h.extend(b"WAVEfmt ");
        h.extend(16u32.to_le_bytes());
        h.extend(1u16.to_le_bytes());
        h.extend(self.channels.to_le_bytes());
        h.extend(self.rate.to_le_bytes());
        h.extend((self.rate * u32::from(self.channels) * 2).to_le_bytes());
        h.extend((self.channels * 2).to_le_bytes());
        h.extend(16u16.to_le_bytes());
        h.extend(b"data");
        h.extend(self.bytes.to_le_bytes());
        self.file
            .seek(SeekFrom::Start(0))
            .and_then(|_| self.file.write_all(&h))
            .and_then(|_| self.file.seek(SeekFrom::End(0)))
            .map_err(|e| e.to_string())?;
        Ok(())
    }
    fn write(&mut self, rate: u32, channels: u16, pcm: &[i16]) -> Result<(), String> {
        if self.rate != rate || self.channels != channels {
            self.configure(rate, channels)?;
        }
        let bytes = pcm
            .len()
            .checked_mul(2)
            .and_then(|n| u32::try_from(n).ok())
            .and_then(|n| self.bytes.checked_add(n))
            .filter(|n| *n <= u32::MAX - 36)
            .ok_or("WAV exceeds RIFF size limit")?;
        let data: Vec<u8> = pcm.iter().flat_map(|s| s.to_le_bytes()).collect();
        self.file.write_all(&data).map_err(|e| e.to_string())?;
        self.bytes = bytes;
        self.header()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn playback() -> Arc<Mutex<Playback>> {
        Arc::new(Mutex::new(Playback {
            frames: VecDeque::new(),
            phase: 0,
            source_rate: 8000,
            output_rate: 48000,
            drain_until: None,
            started: false,
            empty_frames: 0,
            gaps: 0,
            missing_frames: 0,
            max_gap_frames: 0,
            preroll_frames: 0,
        }))
    }
    #[test]
    fn speaker_starvation_is_distinct_from_logical_pcm_and_ignores_idle() {
        let queue = playback();
        let mut output = Output {
            playback: Some(queue.clone()),
            ..Output::default()
        };
        // No logical renderer is involved: these are the actual sink's samples.
        for _ in 0..480 {
            queue.lock().unwrap().next();
        }
        output.write(8000, 1, &[123]).unwrap();
        assert_eq!(output.statistics(), (0, 0, 0));
        for _ in 0..486 {
            queue.lock().unwrap().next();
        }
        assert_eq!(queue.lock().unwrap().empty_frames, 480);
        output.write(8000, 1, &[456]).unwrap();
        assert_eq!(output.statistics(), (1, 480, 10));
        for _ in 0..966 {
            queue.lock().unwrap().next();
        }
        output.write(8000, 1, &[789]).unwrap();
        assert_eq!(output.statistics(), (2, 1440, 20));
        for _ in 0..48000 {
            queue.lock().unwrap().next();
        }
        output.clear_playback(); // Normal final drain/pause/stop.
        for _ in 0..48000 {
            queue.lock().unwrap().next();
        }
        output.write(8000, 1, &[0]).unwrap();
        assert_eq!(output.statistics(), (2, 1440, 20));
    }
    #[test]
    fn speaker_preroll_absorbs_jitter_and_is_reapplied_after_pause() {
        let queue = playback();
        queue.lock().unwrap().preroll_frames = 320; // 40 ms at 8 kHz.
        let mut output = Output { playback: Some(queue.clone()), ..Output::default() };
        output.write(8000, 1, &[1000; 80]).unwrap();
        assert_eq!(queue.lock().unwrap().frames.len(), 400);
        for _ in 0..960 { queue.lock().unwrap().next(); } // 20 ms.
        output.write(8000, 1, &[1000; 80]).unwrap();
        assert_eq!(output.statistics(), (0, 0, 0));
        output.clear_playback();
        output.write(8000, 1, &[1000; 10]).unwrap();
        assert_eq!(queue.lock().unwrap().frames.len(), 330);
    }
}
