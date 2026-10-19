//! Ethernet NIC shared by the VM, BF CPU host and native runtime.
//! Only mailbox calls touch guest RAM. Host workers own bounded frame queues.
mod backend;
mod host;
#[cfg(target_os = "macos")]
mod vmnet;
#[cfg(test)]
mod tests;
use crate::devices::{guest_range, GuestMemory, STATUS_BAD_DESCRIPTOR};
pub use backend::NetworkBackend;
use serde::{Deserialize, Deserializer, Serialize, Serializer};
use std::{
    collections::VecDeque,
    ops::Range,
    sync::{Arc, Mutex},
};

pub const CAP_NETWORK: u16 = 0x0020;
pub const COMMAND_NET_INFO: u16 = 0x0100;
pub const COMMAND_NET_TX: u16 = 0x0101;
pub const COMMAND_NET_RX: u16 = 0x0102;
pub const COMMAND_NET_STATS: u16 = 0x0103;
pub const NET_OK: u16 = 0;
pub const NET_EMPTY: u16 = 1;
pub const NET_WOULD_BLOCK: u16 = 2;
pub const NET_BUFFER_TOO_SMALL: u16 = 3;
pub const NET_LINK_DOWN: u16 = 4;
pub const MAX_FRAME: usize = 1514;
pub const QUEUE_FRAMES: usize = 64;
pub const QUEUE_OCTETS: usize = 65536;

pub fn descriptor_length(command: u16) -> Option<usize> {
    match command {
        COMMAND_NET_INFO => Some(18),
        COMMAND_NET_TX => Some(8),
        COMMAND_NET_RX => Some(9),
        COMMAND_NET_STATS => Some(27),
        _ => None,
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Counters {
    rx_received: u32,
    rx_delivered: u32,
    rx_dropped: u32,
    tx_accepted: u32,
    tx_sent: u32,
    tx_failed: u32,
    tx_would_block: u32,
    rx_oversized: u32,
    backend_errors: u32,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct State {
    #[serde(default)]
    generation: u64,
    mac: [u8; 6],
    link: bool,
    live: bool,
    rx: VecDeque<Vec<u8>>,
    tx: VecDeque<Vec<u8>>,
    counters: Counters,
}
impl Default for State {
    fn default() -> Self {
        Self {
            generation: 0,
            mac: [2, 0, 0, 0, 0, 1],
            link: false,
            live: false,
            rx: VecDeque::new(),
            tx: VecDeque::new(),
            counters: Counters::default(),
        }
    }
}
fn octets(queue: &VecDeque<Vec<u8>>) -> usize {
    queue.iter().map(Vec::len).sum()
}
fn room(queue: &VecDeque<Vec<u8>>, length: usize) -> bool {
    queue.len() < QUEUE_FRAMES && octets(queue) + length <= QUEUE_OCTETS
}
fn valid_mac(mac: &[u8; 6]) -> bool {
    mac[0] & 1 == 0 && *mac != [0; 6]
}
fn valid_frame(frame: &[u8]) -> bool {
    (14..=MAX_FRAME).contains(&frame.len())
}

/// Cloning/checkpointing takes an independent snapshot, never a second NIC alias.
#[derive(Debug, Default)]
pub struct NetworkDevice {
    state: Arc<Mutex<State>>,
    host: host::HostNetwork,
}
impl Clone for NetworkDevice {
    fn clone(&self) -> Self {
        Self {
            state: Arc::new(Mutex::new(self.state.lock().unwrap().clone())),
            host: host::HostNetwork::default(),
        }
    }
}
impl Serialize for NetworkDevice {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let state = self.state.lock().unwrap();
        if state.live {
            return Err(serde::ser::Error::custom(
                "live network checkpoints are unsupported; detach the backend first",
            ));
        }
        state.serialize(serializer)
    }
}
impl<'de> Deserialize<'de> for NetworkDevice {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let state = State::deserialize(deserializer)?;
        let device = Self {
            state: Arc::new(Mutex::new(state)),
            host: host::HostNetwork::default(),
        };
        device
            .validate_checkpoint()
            .map_err(serde::de::Error::custom)?;
        Ok(device)
    }
}

/// Host-only queue access. This handle cannot read or write guest memory/status.
#[derive(Clone)]
pub struct NetworkPort(Arc<Mutex<State>>);
/// An owned transmission. Completion is associated with its reset generation.
pub struct OutgoingFrame {
    pub octets: Vec<u8>,
    generation: u64,
}
impl NetworkPort {
    pub fn receive(&self, frame: Vec<u8>) {
        let mut state = self.0.lock().unwrap();
        state.counters.rx_received = state.counters.rx_received.saturating_add(1);
        if !valid_frame(&frame) || !room(&state.rx, frame.len()) {
            state.counters.rx_dropped = state.counters.rx_dropped.saturating_add(1);
            if frame.len() > MAX_FRAME {
                state.counters.rx_oversized = state.counters.rx_oversized.saturating_add(1);
            }
        } else {
            state.rx.push_back(frame);
        }
    }
    pub fn take_transmit(&self) -> Option<OutgoingFrame> {
        let mut state = self.0.lock().unwrap();
        let generation = state.generation;
        state
            .tx
            .pop_front()
            .map(|octets| OutgoingFrame { octets, generation })
    }
    pub fn transmitted(&self, frame: OutgoingFrame, success: bool) {
        let mut state = self.0.lock().unwrap();
        // Reset cancels old accounting, even if host I/O was already in flight.
        if frame.generation != state.generation {
            return;
        }
        let counter = if success {
            &mut state.counters.tx_sent
        } else {
            &mut state.counters.tx_failed
        };
        *counter = counter.saturating_add(1);
    }
    pub fn backend_failed(&self) {
        let mut state = self.0.lock().unwrap();
        state.link = false;
        state.counters.backend_errors = state.counters.backend_errors.saturating_add(1);
        state.counters.tx_failed = state
            .counters
            .tx_failed
            .saturating_add(state.tx.len() as u32);
        state.tx.clear();
    }
}

impl NetworkDevice {
    /// Runtime policy, separate from serializable guest state. Default CLI
    /// policy is lazy host-only Ethernet on macOS; library devices stay offline.
    pub fn configure_host(&mut self, setting: Option<&std::ffi::OsStr>) -> Result<(), String> {
        if self.state.lock().unwrap().live {
            return Err("cannot reconfigure an attached live NIC".into());
        }
        self.host = host::HostNetwork::new(setting);
        if self.host.is_external() {
            self.host.ensure(self)?;
        }
        Ok(())
    }
    /// Attach before entering an interactive terminal, so helper authorization
    /// cannot compete with the guest's input reader. Ordinary attachment is lazy.
    pub fn prepare_host(&self) -> Result<(), String> {
        self.host.ensure(self)
    }
    pub fn port(&self) -> NetworkPort {
        NetworkPort(Arc::clone(&self.state))
    }
    /// Attach a controlled peer. Live backends use the same queues but disallow
    /// exact checkpoint claims while the external connection exists.
    pub fn set_link(&self, mac: [u8; 6], up: bool) -> Result<(), String> {
        if !valid_mac(&mac) {
            return Err("network MAC must be nonzero and unicast".into());
        }
        let mut state = self.state.lock().unwrap();
        if state.live {
            return Err("cannot reconfigure an attached live NIC".into());
        }
        state.mac = mac;
        state.link = up;
        Ok(())
    }
    pub fn validate_checkpoint(&self) -> Result<(), String> {
        let state = self.state.lock().unwrap();
        if state.live
            || !valid_mac(&state.mac)
            || [&state.rx, &state.tx].iter().any(|q| {
                q.len() > QUEUE_FRAMES
                    || octets(q) > QUEUE_OCTETS
                    || q.iter().any(|f| !valid_frame(f))
            })
        {
            return Err("invalid or live network checkpoint".into());
        }
        Ok(())
    }
    pub(crate) fn reset(&self) {
        let mut state = self.state.lock().unwrap();
        state.generation = state.generation.wrapping_add(1);
        state.rx.clear();
        state.tx.clear();
        state.counters = Counters::default();
    }
    pub(crate) fn execute<M: GuestMemory + ?Sized>(
        &self,
        command: u16,
        memory: &mut M,
        descriptor: Range<usize>,
        bank_size: u16,
    ) -> Result<(), u16> {
        let mut d: Vec<u16> = descriptor.clone().map(|i| memory.read(i)).collect();
        if d[2] != 0 {
            return Err(STATUS_BAD_DESCRIPTOR);
        }
        // CAPS and unrelated devices never start host networking. An initial
        // valid NET_INFO explicitly queries the NIC and enables lazy attachment.
        if command == COMMAND_NET_INFO {
            let _ = self.host.ensure(self); // INFO reports link down while attachment recovers
        }
        let mut state = self.state.lock().unwrap();
        match command {
            COMMAND_NET_INFO => {
                d[3] = u16::from(state.link);
                d[4] = 1500;
                d[5] = MAX_FRAME as u16;
                for (word, &octet) in d[6..12].iter_mut().zip(state.mac.iter()) {
                    *word = u16::from(octet);
                }
                d[12] = QUEUE_FRAMES as u16;
                d[13] = QUEUE_FRAMES as u16;
                put32(&mut d, 14, QUEUE_OCTETS as u32);
                put32(&mut d, 16, QUEUE_OCTETS as u32);
            }
            COMMAND_NET_TX | COMMAND_NET_RX => {
                if *d.last().unwrap() != 0 {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                let length = usize::from(d[5]);
                if command == COMMAND_NET_TX && !(14..=MAX_FRAME).contains(&length) {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                let buffer = guest_range(memory.len(), bank_size, d[3], d[4], length)?;
                if buffer.start < descriptor.end && descriptor.start < buffer.end {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                if command == COMMAND_NET_TX {
                    // Validate even while link-down/full: malformed input must
                    // not be hidden by a transient queue outcome.
                    let frame: Vec<u8> = buffer
                        .map(|i| u8::try_from(memory.read(i)))
                        .collect::<Result<_, _>>()
                        .map_err(|_| STATUS_BAD_DESCRIPTOR)?;
                    d[6] = if !state.link {
                        NET_LINK_DOWN
                    } else if !room(&state.tx, frame.len()) {
                        state.counters.tx_would_block =
                            state.counters.tx_would_block.saturating_add(1);
                        NET_WOULD_BLOCK
                    } else {
                        state.tx.push_back(frame);
                        state.counters.tx_accepted = state.counters.tx_accepted.saturating_add(1);
                        NET_OK
                    };
                } else {
                    d[6] = state.rx.front().map_or(0, |f| f.len() as u16);
                    d[7] = if let Some(frame) = state.rx.front() {
                        if frame.len() > length {
                            NET_BUFFER_TOO_SMALL
                        } else {
                            let frame = state.rx.pop_front().unwrap();
                            for (index, octet) in frame.into_iter().enumerate() {
                                memory.write(buffer.start + index, u16::from(octet));
                            }
                            state.counters.rx_delivered =
                                state.counters.rx_delivered.saturating_add(1);
                            NET_OK
                        }
                    } else if state.link {
                        NET_EMPTY
                    } else {
                        NET_LINK_DOWN
                    };
                }
            }
            COMMAND_NET_STATS => {
                d[3] = state.rx.len() as u16;
                d[4] = state.tx.len() as u16;
                put32(&mut d, 5, octets(&state.rx) as u32);
                put32(&mut d, 7, octets(&state.tx) as u32);
                let c = &state.counters;
                for (i, value) in [
                    c.rx_received,
                    c.rx_delivered,
                    c.rx_dropped,
                    c.tx_accepted,
                    c.tx_sent,
                    c.tx_failed,
                    c.tx_would_block,
                    c.rx_oversized,
                    c.backend_errors,
                ]
                .into_iter()
                .enumerate()
                {
                    put32(&mut d, 9 + i * 2, value);
                }
            }
            _ => unreachable!("network command dispatch was validated"),
        }
        for (index, value) in descriptor.zip(d) {
            memory.write(index, value);
        }
        Ok(())
    }
}
fn put32(d: &mut [u16], index: usize, value: u32) {
    d[index] = value as u16;
    d[index + 1] = (value >> 16) as u16;
}
