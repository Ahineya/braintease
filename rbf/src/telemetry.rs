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

/// Optional runner observer. Completion status is emitted even between intervals.
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
    lite: Option<Lite>,
}

#[derive(Default)]
struct Lite {
    started_at: Option<Duration>,
    dot_slot: u64,
    summary_slot: u64,
    dots_pending: bool,
}

impl<W: Write> Telemetry<W> {
    pub fn new(writer: W, interval: Duration) -> Result<Self, String> {
        let started = Instant::now();
        Self::with_clock(writer, interval, move || started.elapsed())
    }

    pub fn new_lite(writer: W) -> Result<Self, String> {
        let started = Instant::now();
        Self::with_lite_clock(writer, move || started.elapsed())
    }

    pub fn with_lite_clock(
        writer: W,
        clock: impl FnMut() -> Duration + Send + 'static,
    ) -> Result<Self, String> {
        let mut telemetry = Self::with_clock(writer, Duration::from_secs(30), clock)?;
        telemetry.lite = Some(Lite::default());
        Ok(telemetry)
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
            lite: None,
        })
    }

    pub fn phase(&mut self, name: &str, details: &str) -> Result<(), String> {
        if self.lite.is_some() {
            return Ok(());
        }
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

    fn update_lite(
        &mut self,
        snapshot: Snapshot,
        status: Option<&str>,
        now: Duration,
    ) -> Result<(), String> {
        let lite = self.lite.as_mut().expect("lite telemetry mode");
        let started = *lite.started_at.get_or_insert_with(|| {
            // Parsing/setup is outside execution. Start the cadence and rate
            // baseline at the runner's first snapshot, like the run report.
            self.emitted_at = now;
            self.emitted_dispatches = snapshot.dispatches;
            self.emitted_instructions = snapshot.instructions;
            now
        });
        if status == Some("started") {
            return Ok(());
        }
        let elapsed = now.saturating_sub(started).as_secs();
        // Anchor to execution start, avoiding cadence drift from polling delay.
        // A late poll emits one dot/summary rather than a burst of stale ones.
        let dot_slot = elapsed / 5;
        let summary_slot = elapsed / 30;
        let dot_due = dot_slot > lite.dot_slot;
        let summary_due = status.is_some() || summary_slot > lite.summary_slot;
        if !dot_due && !summary_due {
            return Ok(());
        }
        let write = (|| -> std::io::Result<()> {
            if dot_due {
                write!(self.writer, ".")?;
                lite.dot_slot = dot_slot;
                lite.dots_pending = true;
            }
            if summary_due {
                if lite.dots_pending {
                    writeln!(self.writer)?;
                    lite.dots_pending = false;
                }
                let seconds = now.saturating_sub(self.emitted_at).as_secs_f64();
                let rate = |count: u64| {
                    if seconds > 0.0 {
                        count as f64 / seconds
                    } else {
                        0.0
                    }
                };
                write!(
                    self.writer,
                    "[{:02}:{:02}:{:02}] BF {} ({}/s) | Guest {} ({}/s)",
                    elapsed / 3600,
                    elapsed / 60 % 60,
                    elapsed % 60,
                    grouped(&snapshot.dispatches.to_string()),
                    grouped(&format!(
                        "{:.0}",
                        rate(snapshot.dispatches.saturating_sub(self.emitted_dispatches))
                    )),
                    grouped(&snapshot.instructions.to_string()),
                    grouped(&format!(
                        "{:.1}",
                        rate(
                            snapshot
                                .instructions
                                .saturating_sub(self.emitted_instructions)
                        )
                    )),
                )?;
                if let Some(status) = status {
                    write!(self.writer, " ({status})")?;
                }
                writeln!(self.writer)?;
            }
            self.writer.flush()
        })();
        write.map_err(|e| format!("Telemetry output: {e}"))?;
        if summary_due {
            lite.summary_slot = summary_slot;
            self.emitted_at = now;
            self.emitted_dispatches = snapshot.dispatches;
            self.emitted_instructions = snapshot.instructions;
        }
        Ok(())
    }
}

/// Group the decimal integers/rates used by the compact display, without
/// rounding large counters or abbreviating away useful progress digits.
fn grouped(value: &str) -> String {
    let (whole, fraction) = value.split_once('.').unwrap_or((value, ""));
    let mut result = String::new();
    for (i, digit) in whole.chars().enumerate() {
        if i != 0 && (whole.len() - i) % 3 == 0 {
            result.push(',');
        }
        result.push(digit);
    }
    if !fraction.is_empty() {
        result.push('.');
        result.push_str(fraction);
    }
    result
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
        if self.lite.is_some() {
            return self.update_lite(snapshot, status, now);
        }
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

    #[derive(Default)]
    struct Recording {
        bytes: Vec<u8>,
        flushes: usize,
    }

    impl Write for Recording {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            self.bytes.extend_from_slice(bytes);
            Ok(bytes.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            self.flushes += 1;
            Ok(())
        }
    }

    #[test]
    fn lite_flushes_dots_and_reports_interval_rates_without_setup_noise() {
        let time = Arc::new(AtomicU64::new(90_000));
        let clock = Arc::clone(&time);
        let mut telemetry = Telemetry::with_lite_clock(Recording::default(), move || {
            Duration::from_millis(clock.load(Ordering::Relaxed))
        })
        .unwrap();
        telemetry.phase("loading", "private/path").unwrap();
        telemetry.phase("parsing", "bytes=100").unwrap();
        let mut state = snapshot();
        telemetry.update(state, Some("started")).unwrap();
        time.store(94_999, Ordering::Relaxed);
        telemetry.update(state, None).unwrap();
        assert!(telemetry.writer.bytes.is_empty());
        assert_eq!(telemetry.writer.flushes, 0);
        for seconds in [5, 10, 15, 20, 25] {
            time.store(90_000 + seconds * 1000, Ordering::Relaxed);
            state.dispatches = seconds * 100_000;
            state.instructions = seconds * 2000;
            telemetry.update(state, None).unwrap();
            // Polling again at the same time must neither print nor flush.
            telemetry.update(state, None).unwrap();
            assert_eq!(telemetry.writer.bytes, vec![b'.'; seconds as usize / 5]);
            assert_eq!(telemetry.writer.flushes, seconds as usize / 5);
        }
        time.store(120_000, Ordering::Relaxed);
        state.dispatches = 3_000_000;
        state.instructions = 60_000;
        telemetry.update(state, None).unwrap();
        assert_eq!(
            String::from_utf8_lossy(&telemetry.writer.bytes),
            "......\n[00:00:30] BF 3,000,000 (100,000/s) | Guest 60,000 (2,000.0/s)\n"
        );
        time.store(150_000, Ordering::Relaxed);
        state.dispatches = 4_500_000;
        state.instructions = 90_000;
        telemetry.update(state, None).unwrap();
        assert!(String::from_utf8_lossy(&telemetry.writer.bytes)
            .ends_with(".\n[00:01:00] BF 4,500,000 (50,000/s) | Guest 90,000 (1,000.0/s)\n"));
        time.store(155_000, Ordering::Relaxed);
        state.dispatches = 4_750_000;
        state.instructions = 90_500;
        telemetry.update(state, Some("host_stop")).unwrap();
        let recording = telemetry.into_inner();
        let text = String::from_utf8(recording.bytes).unwrap();
        assert!(text.ends_with(
            ".\n[00:01:05] BF 4,750,000 (50,000/s) | Guest 90,500 (100.0/s) (host_stop)\n"
        ));
        assert_eq!(recording.flushes, 8);
        assert!(!text.contains("phase="));
        assert!(!text.contains("private/path"));
    }

    #[test]
    fn lite_late_polls_do_not_drift_or_replay_missed_intervals() {
        let time = Arc::new(AtomicU64::new(0));
        let clock = Arc::clone(&time);
        let mut telemetry = Telemetry::with_lite_clock(Vec::new(), move || {
            Duration::from_secs(clock.load(Ordering::Relaxed))
        })
        .unwrap();
        let state = snapshot();
        telemetry.update(state, Some("started")).unwrap();
        for seconds in [31, 60, 3605] {
            time.store(seconds, Ordering::Relaxed);
            telemetry.update(state, None).unwrap();
        }
        assert_eq!(
            String::from_utf8(telemetry.into_inner()).unwrap(),
            ".\n[00:00:31] BF 0 (0/s) | Guest 0 (0.0/s)\n\
             .\n[00:01:00] BF 0 (0/s) | Guest 0 (0.0/s)\n\
             .\n[01:00:05] BF 0 (0/s) | Guest 0 (0.0/s)\n"
        );
    }

    #[test]
    fn lite_finishes_short_runs_and_preserves_large_counters() {
        let mut telemetry = Telemetry::with_lite_clock(Vec::new(), || Duration::ZERO).unwrap();
        let mut state = snapshot();
        telemetry.update(state, Some("started")).unwrap();
        state.dispatches = u64::MAX;
        state.instructions = 999;
        telemetry.update(state, Some("halted")).unwrap();
        assert_eq!(
            String::from_utf8(telemetry.into_inner()).unwrap(),
            "[00:00:00] BF 18,446,744,073,709,551,615 (0/s) | Guest 999 (0.0/s) (halted)\n"
        );
    }

    #[test]
    fn lite_dot_and_summary_write_or_flush_errors_are_explicit() {
        struct Broken {
            on_flush: bool,
        }
        impl Write for Broken {
            fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
                if self.on_flush {
                    Ok(bytes.len())
                } else {
                    Err(std::io::Error::other("closed"))
                }
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Err(std::io::Error::other("closed"))
            }
        }
        for on_flush in [false, true] {
            for final_status in [None, Some("halted")] {
                let time = Arc::new(AtomicU64::new(0));
                let clock = Arc::clone(&time);
                let mut telemetry = Telemetry::with_lite_clock(Broken { on_flush }, move || {
                    Duration::from_secs(clock.load(Ordering::Relaxed))
                })
                .unwrap();
                telemetry.update(snapshot(), Some("started")).unwrap();
                time.store(5, Ordering::Relaxed);
                assert_eq!(
                    telemetry.update(snapshot(), final_status).unwrap_err(),
                    "Telemetry output: closed"
                );
            }
        }
    }
}
