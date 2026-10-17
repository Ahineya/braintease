//! Host clocks, independent of the runner's deterministic simulation ticks.
use serde::{Deserialize, Serialize};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HostClock {
    elapsed_ms: u64,
    started: bool,
    // Start lazily, so guests which only use logical time have unchanged snapshots.
    #[serde(skip)]
    anchor: Option<Instant>,
}

impl HostClock {
    pub fn monotonic(&mut self) -> Duration {
        self.started = true;
        let anchor = self.anchor.get_or_insert_with(Instant::now);
        Duration::from_millis(self.elapsed_ms).saturating_add(anchor.elapsed())
    }

    pub fn realtime() -> Result<Duration, u16> {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|_| crate::devices::STATUS_IO)
    }

    pub fn checkpoint(&self) -> Self {
        let elapsed = Duration::from_millis(self.elapsed_ms)
            .saturating_add(self.anchor.map_or(Duration::ZERO, |a| a.elapsed()));
        Self {
            elapsed_ms: elapsed.as_millis().min(u128::from(u64::MAX)) as u64,
            started: self.started,
            anchor: None,
        }
    }

    pub fn resume(&mut self) {
        self.anchor = self.started.then(Instant::now);
    }
}

/// Whole seconds (64 bits), nanoseconds and reported resolution (32 bits each).
/// Millisecond quantization avoids claiming more precision than the ABI promises.
pub fn write_time(descriptor: &mut [u16], time: Duration) {
    let seconds = time.as_secs();
    let nanos = time.subsec_millis() * 1_000_000;
    for i in 0..4 {
        descriptor[2 + i] = (seconds >> (i * 16)) as u16;
    }
    descriptor[6] = nanos as u16;
    descriptor[7] = (nanos >> 16) as u16;
    descriptor[8] = 1_000_000u32 as u16;
    descriptor[9] = (1_000_000u32 >> 16) as u16;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn full_width_wire_values_and_checkpoint_reanchoring() {
        let mut words = [1; 10];
        write_time(
            &mut words,
            Duration::new(0x1234_5678_9abc_def0, 987_654_321),
        );
        assert_eq!(&words[2..6], &[0xdef0, 0x9abc, 0x5678, 0x1234]);
        assert_eq!(u32::from(words[6]) | u32::from(words[7]) << 16, 987_000_000);
        assert_eq!(u32::from(words[8]) | u32::from(words[9]) << 16, 1_000_000);

        let clock = HostClock {
            elapsed_ms: u64::from(u32::MAX) + 1234,
            started: true,
            anchor: Some(Instant::now() - Duration::from_secs(5)),
        };
        let saved = clock.checkpoint();
        assert!(saved.elapsed_ms >= u64::from(u32::MAX) + 6234);
        assert!(saved.anchor.is_none());
        let mut restored: HostClock =
            serde_json::from_str(&serde_json::to_string(&saved).unwrap()).unwrap();
        assert!(restored.anchor.is_none());
        let resumed = restored.monotonic();
        assert!(resumed >= Duration::from_millis(saved.elapsed_ms));
        assert!(resumed < Duration::from_millis(saved.elapsed_ms + 1000));
    }
}
