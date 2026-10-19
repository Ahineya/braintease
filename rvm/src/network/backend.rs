//! Local raw-Ethernet transport. A separate helper owns host-specific networking.
use super::*;
use std::{
    path::Path,
    sync::atomic::{AtomicBool, Ordering},
    thread,
};

#[cfg(unix)]
fn socket_backpressure(error: &std::io::Error) -> bool {
    error.kind() == std::io::ErrorKind::WouldBlock
        || error.kind() == std::io::ErrorKind::Interrupted
        || error.raw_os_error() == Some(libc::ENOBUFS)
}
#[cfg(unix)]
use std::{
    sync::atomic::AtomicU64,
    time::{Duration, Instant},
};

pub struct NetworkBackend {
    stop: Arc<AtomicBool>,
    worker: Option<thread::JoinHandle<()>>,
    device: NetworkPort,
    directory: std::path::PathBuf,
}
impl NetworkBackend {
    /// Connect to a local helper's Unix datagram socket. RNET v1 negotiates
    /// MAC/MTU; subsequent datagrams are complete Ethernet frames, without FCS.
    pub fn connect(device: &NetworkDevice, path: &Path) -> Result<Self, String> {
        #[cfg(unix)]
        {
            Self::connect_unix(device, path)
        }
        #[cfg(not(unix))]
        {
            let _ = (device, path);
            Err("Unix Ethernet backends are unavailable on this host".into())
        }
    }
    #[cfg(unix)]
    fn connect_unix(device: &NetworkDevice, path: &Path) -> Result<Self, String> {
        use std::{
            io::ErrorKind,
            os::unix::{fs::DirBuilderExt, net::UnixDatagram},
        };
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let directory = std::env::temp_dir().join(format!(
            "ripple-net-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::DirBuilder::new()
            .mode(0o700)
            .create(&directory)
            .map_err(|e| format!("network private directory: {e}"))?;
        let setup = || -> Result<UnixDatagram, String> {
            let socket = UnixDatagram::bind(directory.join("nic.sock"))
                .map_err(|e| format!("network bind: {e}"))?;
            socket
                .connect(path)
                .map_err(|e| format!("network connect {}: {e}", path.display()))?;
            socket
                .set_read_timeout(Some(Duration::from_secs(2)))
                .map_err(|e| e.to_string())?;
            socket
                .set_write_timeout(Some(Duration::from_secs(2)))
                .map_err(|e| e.to_string())?;
            socket
                .send(b"RNET\x01")
                .map_err(|e| format!("network hello: {e}"))?;
            let mut info = [0; 64];
            let count = socket
                .recv(&mut info)
                .map_err(|e| format!("network handshake: {e}"))?;
            if count != 13
                || &info[..5] != b"RNET\x01"
                || u16::from_be_bytes([info[11], info[12]]) != 1500
            {
                return Err("network helper must provide RNET v1 Ethernet with MTU 1500".into());
            }
            let mac: [u8; 6] = info[5..11].try_into().unwrap();
            if !valid_mac(&mac) {
                return Err("network helper supplied an invalid MAC".into());
            }
            socket.set_nonblocking(true).map_err(|e| e.to_string())?;
            let mut state = device.state.lock().unwrap();
            if state.live || !state.rx.is_empty() || !state.tx.is_empty() {
                return Err("attach a live backend only to an idle, disconnected NIC".into());
            }
            state.mac = mac;
            state.link = true;
            state.live = true;
            Ok(socket)
        };
        let socket = match setup() {
            Ok(socket) => socket,
            Err(error) => {
                let _ = std::fs::remove_file(directory.join("nic.sock"));
                let _ = std::fs::remove_dir(&directory);
                return Err(error);
            }
        };
        let stop = Arc::new(AtomicBool::new(false));
        let stopping = Arc::clone(&stop);
        let port = device.port();
        let background = port.clone();
        let worker = thread::Builder::new()
            .name("ripple-network".into())
            .spawn(move || {
                let mut buffer = [0; 65536];
                let mut pending: Option<OutgoingFrame> = None;
                let mut heartbeat = Instant::now();
                let mut heard = Instant::now();
                let result = (|| -> std::io::Result<()> {
                    while !stopping.load(Ordering::Relaxed) {
                        for _ in 0..64 {
                            match socket.recv(&mut buffer) {
                                Ok(count) => {
                                    if count == 5 && &buffer[..5] == b"RNET\x02" {
                                        heard = Instant::now();
                                    } else if count == 5 && &buffer[..5] == b"RNET\x03" {
                                        return Err(std::io::Error::other(
                                            "network helper reported a link failure",
                                        ));
                                    } else {
                                        if valid_frame(&buffer[..count]) {
                                            heard = Instant::now();
                                        }
                                        background.receive(buffer[..count].to_vec());
                                    }
                                }
                                Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                                Err(e) if socket_backpressure(&e) => break,
                                Err(e) => return Err(e),
                            }
                        }
                        for _ in 0..64 {
                            if pending.is_none() {
                                pending = background.take_transmit();
                            }
                            let Some(frame) = pending.as_ref() else {
                                break;
                            };
                            match socket.send(&frame.octets) {
                                Ok(n) if n == frame.octets.len() => {
                                    background.transmitted(pending.take().unwrap(), true);
                                }
                                Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                                Err(e) if socket_backpressure(&e) => break,
                                other => {
                                    return Err(std::io::Error::other(format!(
                                        "network transmit: {other:?}"
                                    )))
                                }
                            }
                        }
                        if heartbeat.elapsed() >= Duration::from_secs(1) {
                            match socket.send(b"RNET\x02") {
                                Ok(5) => heartbeat = Instant::now(),
                                Err(e) if socket_backpressure(&e) => {}
                                other => {
                                    return Err(std::io::Error::other(format!(
                                        "network heartbeat: {other:?}"
                                    )))
                                }
                            }
                        }
                        if heard.elapsed() > Duration::from_secs(5) {
                            return Err(std::io::Error::other(
                                "network helper heartbeat timed out",
                            ));
                        }
                        thread::sleep(Duration::from_millis(1));
                    }
                    Ok(())
                })();
                if let Some(frame) = pending {
                    background.transmitted(frame, false);
                }
                if result.is_ok() {
                    let _ = socket.send(b"RNET\x04"); // orderly detach; no guest RAM involved
                }
                if let Err(error) = result {
                    eprintln!("network backend: {error}");
                    background.backend_failed();
                }
            })
            .map_err(|error| {
                let mut state = port.0.lock().unwrap();
                state.live = false;
                state.link = false;
                let _ = std::fs::remove_file(directory.join("nic.sock"));
                let _ = std::fs::remove_dir(&directory);
                format!("network worker: {error}")
            })?;
        Ok(Self {
            stop,
            worker: Some(worker),
            device: port,
            directory,
        })
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;

    #[test]
    fn transient_socket_buffer_pressure_is_not_a_dead_backend() {
        for code in [libc::EAGAIN, libc::EINTR, libc::ENOBUFS] {
            assert!(socket_backpressure(&std::io::Error::from_raw_os_error(
                code
            )));
        }
        for code in [libc::ECONNREFUSED, libc::ECONNRESET, libc::ENOENT] {
            assert!(!socket_backpressure(&std::io::Error::from_raw_os_error(
                code
            )));
        }
    }
}
impl Drop for NetworkBackend {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(worker) = self.worker.take() {
            worker.join().expect("network worker panicked");
        }
        let mut state = self.device.0.lock().unwrap();
        state.live = false;
        state.link = false;
        state.counters.tx_failed = state
            .counters
            .tx_failed
            .saturating_add(state.tx.len() as u32);
        state.tx.clear();
        drop(state);
        let _ = std::fs::remove_file(self.directory.join("nic.sock"));
        let _ = std::fs::remove_dir(&self.directory);
    }
}
