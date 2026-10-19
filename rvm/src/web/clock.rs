//! Host samples are refreshed by bounded browser execution turns.
use std::time::Duration;
#[derive(Debug, Clone, Default, serde::Serialize, serde::Deserialize)]
pub struct HostClock;
impl HostClock {
    pub fn monotonic(&mut self) -> Duration {
        Duration::from_millis(crate::web::now_ms() as u64)
    }
    pub fn realtime() -> Result<Duration, u16> {
        Ok(Duration::from_millis(crate::web::epoch_ms() as u64))
    }
    pub fn checkpoint(&self) -> Self {
        self.clone()
    }
    pub fn resume(&mut self) {}
}
pub fn write_time(d: &mut [u16], t: Duration) {
    let s = t.as_secs();
    let n = t.subsec_millis() * 1_000_000;
    for i in 0..4 {
        d[2 + i] = (s >> (i * 16)) as u16;
    }
    d[6] = n as u16;
    d[7] = (n >> 16) as u16;
    d[8] = 1_000_000u32 as u16;
    d[9] = (1_000_000u32 >> 16) as u16;
}
