//! Optional progress reporting. Clock and output are injectable; no device or
//! interpreter work happens here. Idle times are sampled at host polling points.
use rvm::runner::ClockPolicy;
use std::{
    io::Write,
    time::{Duration, Instant},
};

#[derive(Clone, Copy, Debug)]
pub struct Snapshot {
    pub dispatches: u64,
    pub instructions: u64,
    pub pc: Option<(u16, u16)>,
    pub frames: u64,
    pub tick: u64,
    pub clock: ClockPolicy,
    pub waiting_for_tick: bool,
    pub last_load: Option<usize>,
    pub last_store: Option<usize>,
    pub framebuffer_writes: u64,
    pub last_framebuffer_store: Option<usize>,
    pub bank_size: u16,
    /// Successful storage data-port transfers, counted in 16-bit disk words.
    pub disk_read_words: u64,
    pub disk_write_words: u64,
    /// Disk block and word addresses use the storage geometry, not RAM banks.
    pub last_disk_read: Option<(u16, u16)>,
    pub disk_position: Option<(u16, u16)>,
}

/// Optional runner observer. A forced status is emitted even between intervals.
pub trait Progress {
    fn update(&mut self, snapshot: Snapshot, status: Option<&str>) -> Result<(), String>;
}

pub fn parse_interval(value: &str) -> Result<Duration, String> {
    let seconds = value
        .parse::<f64>()
        .map_err(|_| "expected seconds between 0.01 and 3600")?;
    if !seconds.is_finite() || !(0.01..=3600.0).contains(&seconds) {
        return Err("telemetry interval must be between 0.01 and 3600 seconds".into());
    }
    Ok(Duration::from_secs_f64(seconds))
}

pub struct Telemetry<W> {
    writer: W,
    clock: Box<dyn FnMut() -> Duration + Send>,
    interval: Duration,
    emitted_at: Duration,
    emitted_dispatches: u64,
    emitted_instructions: u64,
    observed_instructions: u64,
    observed_frames: u64,
    instruction_at: Option<Duration>,
    frame_at: Option<Duration>,
    emitted_disk_reads: u64,
    observed_disk_reads: u64,
    observed_disk_writes: u64,
    disk_at: Option<Duration>,
}

impl<W: Write> Telemetry<W> {
    pub fn new(writer: W, interval: Duration) -> Result<Self, String> {
        let started = Instant::now();
        Self::with_clock(writer, interval, move || started.elapsed())
    }

    pub fn with_clock(
        writer: W,
        interval: Duration,
        clock: impl FnMut() -> Duration + Send + 'static,
    ) -> Result<Self, String> {
        if interval.is_zero() {
            return Err("telemetry interval must be positive".into());
        }
        Ok(Self {
            writer,
            clock: Box::new(clock),
            interval,
            emitted_at: Duration::ZERO,
            emitted_dispatches: 0,
            emitted_instructions: 0,
            observed_instructions: 0,
            observed_frames: 0,
            instruction_at: None,
            frame_at: None,
            emitted_disk_reads: 0,
            observed_disk_reads: 0,
            observed_disk_writes: 0,
            disk_at: None,
        })
    }

    pub fn phase(&mut self, name: &str, details: &str) -> Result<(), String> {
        let now = (self.clock)();
        writeln!(
            self.writer,
            "rbf telemetry elapsed={:.3}s phase={name} {details}",
            now.as_secs_f64()
        )
        .and_then(|()| self.writer.flush())
        .map_err(|e| format!("Telemetry output: {e}"))
    }

    pub fn into_inner(self) -> W {
        self.writer
    }
}

fn age(now: Duration, changed: Option<Duration>) -> String {
    changed.map_or_else(
        || "none-yet".into(),
        |at| format!("{:.3}s", now.saturating_sub(at).as_secs_f64()),
    )
}

fn address(value: Option<usize>, bank_size: u16) -> String {
    value.map_or_else(
        || "none".into(),
        |value| {
            let bank_size = usize::from(bank_size).max(1);
            format!("{}:{}", value / bank_size, value % bank_size)
        },
    )
}

fn disk_address(value: Option<(u16, u16)>) -> String {
    value.map_or_else(|| "none".into(), |(block, word)| format!("{block}:{word}"))
}

impl<W: Write> Progress for Telemetry<W> {
    fn update(&mut self, snapshot: Snapshot, status: Option<&str>) -> Result<(), String> {
        let now = (self.clock)();
        if snapshot.instructions != self.observed_instructions {
            self.observed_instructions = snapshot.instructions;
            self.instruction_at = Some(now);
        }
        if snapshot.frames != self.observed_frames {
            self.observed_frames = snapshot.frames;
            self.frame_at = Some(now);
        }
        if snapshot.disk_read_words != self.observed_disk_reads
            || snapshot.disk_write_words != self.observed_disk_writes
        {
            self.observed_disk_reads = snapshot.disk_read_words;
            self.observed_disk_writes = snapshot.disk_write_words;
            self.disk_at = Some(now);
        }
        let elapsed = now.saturating_sub(self.emitted_at);
        if status.is_none() && elapsed < self.interval {
            return Ok(());
        }
        let seconds = elapsed.as_secs_f64();
        let rate = |count: u64| {
            if seconds > 0.0 {
                count as f64 / seconds
            } else {
                0.0
            }
        };
        let phase = if snapshot.waiting_for_tick {
            "waiting-clock"
        } else if snapshot.pc.is_none() {
            "cpu-init"
        } else if snapshot.frames == 0 {
            "running-first-frame-pending"
        } else {
            "running"
        };
        let clock = match snapshot.clock {
            ClockPolicy::Unthrottled => "unthrottled",
            ClockPolicy::Paced => "paced",
            ClockPolicy::Stepped => "stepped",
        };
        let pc = snapshot
            .pc
            .map_or_else(|| "none".into(), |(bank, pc)| format!("{bank}:{pc}"));
        writeln!(self.writer,
            "rbf telemetry elapsed={:.3}s phase={phase} status={} bf={} bf/s={:.0} guest={} guest/s={:.1} pc={pc} guest_idle~={} frames={} frame_idle~={} tick={} clock={clock} load={} store={} legacy_fb_writes={} last_legacy_fb_store={} disk_read_words={} disk_read_words/s={:.1} disk_write_words={} disk_last_read={} disk_pos={} disk_idle~={}",
            now.as_secs_f64(), status.unwrap_or("running"), snapshot.dispatches,
            rate(snapshot.dispatches.saturating_sub(self.emitted_dispatches)), snapshot.instructions,
            rate(snapshot.instructions.saturating_sub(self.emitted_instructions)), age(now, self.instruction_at),
            snapshot.frames, age(now, self.frame_at), snapshot.tick,
            address(snapshot.last_load, snapshot.bank_size), address(snapshot.last_store, snapshot.bank_size),
            snapshot.framebuffer_writes, address(snapshot.last_framebuffer_store, snapshot.bank_size),
            snapshot.disk_read_words,
            rate(snapshot.disk_read_words.saturating_sub(self.emitted_disk_reads)), snapshot.disk_write_words,
            disk_address(snapshot.last_disk_read), disk_address(snapshot.disk_position), age(now, self.disk_at))
            .and_then(|()| self.writer.flush()).map_err(|e| format!("Telemetry output: {e}"))?;
        self.emitted_at = now;
        self.emitted_dispatches = snapshot.dispatches;
        self.emitted_instructions = snapshot.instructions;
        self.emitted_disk_reads = snapshot.disk_read_words;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    };

    fn snapshot() -> Snapshot {
        Snapshot {
            dispatches: 0,
            instructions: 0,
            pc: None,
            frames: 0,
            tick: 0,
            clock: ClockPolicy::Unthrottled,
            waiting_for_tick: false,
            last_load: None,
            last_store: None,
            framebuffer_writes: 0,
            last_framebuffer_store: None,
            bank_size: 64,
            disk_read_words: 0,
            disk_write_words: 0,
            last_disk_read: None,
            disk_position: None,
        }
    }

    #[test]
    fn fake_clock_covers_interval_rates_idle_time_and_final_flush() {
        let time = Arc::new(AtomicU64::new(0));
        let clock = Arc::clone(&time);
        let mut telemetry = Telemetry::with_clock(Vec::new(), Duration::from_secs(1), move || {
            Duration::from_millis(clock.load(Ordering::Relaxed))
        })
        .unwrap();
        telemetry.phase("parsing", "bytes=100").unwrap();
        let mut state = snapshot();
        telemetry.update(state, Some("started")).unwrap();
        time.store(500, Ordering::Relaxed);
        state.dispatches = 100;
        telemetry.update(state, None).unwrap();
        time.store(1000, Ordering::Relaxed);
        state.dispatches = 200;
        state.instructions = 2;
        state.pc = Some((1, 3));
        state.last_load = Some(65);
        state.disk_read_words = 8;
        state.last_disk_read = Some((63, 32767));
        state.disk_position = Some((63, 32768));
        telemetry.update(state, None).unwrap();
        time.store(2500, Ordering::Relaxed);
        state.waiting_for_tick = true;
        state.clock = ClockPolicy::Stepped;
        telemetry.update(state, None).unwrap();
        time.store(2600, Ordering::Relaxed);
        state.frames = 1;
        state.disk_write_words = 1;
        telemetry.update(state, Some("halted")).unwrap();
        let output = String::from_utf8(telemetry.into_inner()).unwrap();
        assert_eq!(output.lines().count(), 5);
        assert!(output.contains("phase=cpu-init"));
        assert!(output.contains("bf=200 bf/s=200 guest=2 guest/s=2.0 pc=1:3"));
        assert!(output.contains("load=1:1"));
        assert!(output.contains("disk_read_words=8 disk_read_words/s=8.0 disk_write_words=0 disk_last_read=63:32767 disk_pos=63:32768 disk_idle~=0.000s"));
        assert!(output.contains("disk_read_words=8 disk_read_words/s=0.0 disk_write_words=0 disk_last_read=63:32767 disk_pos=63:32768 disk_idle~=1.500s"));
        assert!(output.contains("disk_read_words=8 disk_read_words/s=0.0 disk_write_words=1 disk_last_read=63:32767 disk_pos=63:32768 disk_idle~=0.000s"));
        assert!(output.contains("phase=waiting-clock"));
        assert!(output.contains("guest_idle~=1.500s"));
        assert!(output.contains("status=halted"));
        assert!(output.contains("frames=1 frame_idle~=0.000s"));
        assert!(!output.contains("NaN"));
    }

    #[test]
    fn interval_validation_and_writer_errors_are_explicit() {
        for value in ["0", "-1", "NaN", "inf", "0.001", "3601", "oops"] {
            assert!(parse_interval(value).is_err(), "{value}");
        }
        assert_eq!(parse_interval("0.25").unwrap(), Duration::from_millis(250));
        struct Broken;
        impl Write for Broken {
            fn write(&mut self, _: &[u8]) -> std::io::Result<usize> {
                Err(std::io::Error::other("closed"))
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }
        let mut telemetry = Telemetry::new(Broken, Duration::from_secs(1)).unwrap();
        assert!(telemetry
            .phase("loading", "")
            .unwrap_err()
            .contains("Telemetry output"));
        assert!(telemetry
            .update(snapshot(), Some("halted"))
            .unwrap_err()
            .contains("closed"));
    }
}
