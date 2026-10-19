//! Real RNET transport failures, without vmnet, sudo or external Internet.
use super::*;
use std::{
    os::unix::net::UnixDatagram,
    sync::atomic::{AtomicBool, AtomicUsize, Ordering},
};

fn until(mut predicate: impl FnMut() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(10);
    while !predicate() {
        assert!(Instant::now() < deadline, "network recovery timed out");
        thread::sleep(Duration::from_millis(5));
    }
}

struct Peer {
    directory: PathBuf,
    fail: Arc<AtomicBool>,
    silent: Arc<AtomicBool>,
    withhold_hello: Arc<AtomicBool>,
    attempts: Arc<AtomicUsize>,
    addresses: Arc<Mutex<Vec<PathBuf>>>,
    stopping: Arc<AtomicBool>,
    server: Option<JoinHandle<()>>,
}
impl Peer {
    fn new(reject_second: bool) -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let directory = std::env::temp_dir().join(format!(
            "rnet-recover-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir(&directory).unwrap();
        let socket = UnixDatagram::bind(directory.join("peer.sock")).unwrap();
        socket
            .set_read_timeout(Some(Duration::from_millis(50)))
            .unwrap();
        let fail = Arc::new(AtomicBool::new(false));
        let silent = Arc::new(AtomicBool::new(false));
        let withhold_hello = Arc::new(AtomicBool::new(false));
        let attempts = Arc::new(AtomicUsize::new(0));
        let stopping = Arc::new(AtomicBool::new(false));
        let addresses = Arc::new(Mutex::new(Vec::new()));
        let (failure, mute, hold, tries, stop, clients) = (
            Arc::clone(&fail),
            Arc::clone(&silent),
            Arc::clone(&withhold_hello),
            Arc::clone(&attempts),
            Arc::clone(&stopping),
            Arc::clone(&addresses),
        );
        let server = thread::spawn(move || {
            let mut buffer = [0; 2048];
            while !stop.load(Ordering::Relaxed) {
                let Ok((count, sender)) = socket.recv_from(&mut buffer) else {
                    continue;
                };
                let address = sender.as_pathname().unwrap();
                if &buffer[..count] == b"RNET\x01" {
                    clients.lock().unwrap().push(address.to_owned());
                    let attempt = tries.fetch_add(1, Ordering::Relaxed);
                    if hold.load(Ordering::Relaxed) {
                        continue;
                    }
                    let reply: &[u8] = if reject_second && attempt == 1 {
                        b"RNET\x03"
                    } else {
                        b"RNET\x01\x02\x03\x04\x05\x06\x07\x05\xdc"
                    };
                    let _ = socket.send_to(reply, address);
                } else if &buffer[..count] == b"RNET\x02" {
                    if mute.load(Ordering::Relaxed) {
                        continue;
                    }
                    let reply: &[u8] = if failure.swap(false, Ordering::Relaxed) {
                        b"RNET\x03"
                    } else {
                        b"RNET\x02"
                    };
                    let _ = socket.send_to(reply, address);
                } else if count >= 14 {
                    let _ = socket.send_to(&buffer[..count], address);
                }
            }
        });
        Self {
            directory,
            fail,
            silent,
            withhold_hello,
            attempts,
            addresses,
            stopping,
            server: Some(server),
        }
    }
    fn device(&self) -> NetworkDevice {
        let mut device = NetworkDevice::default();
        device
            .configure_host(Some(self.directory.join("peer.sock").as_os_str()))
            .unwrap();
        device
    }
}
impl Drop for Peer {
    fn drop(&mut self) {
        self.stopping.store(true, Ordering::Relaxed);
        self.server.take().unwrap().join().unwrap();
        std::fs::remove_file(self.directory.join("peer.sock")).unwrap();
        std::fs::remove_dir(&self.directory).unwrap();
    }
}

// Exercise guest-visible INFO, not merely the internal host manager result.
fn link(device: &NetworkDevice) -> bool {
    let mut ram = vec![0; 18];
    ram[0] = 1;
    ram[1] = 18;
    device
        .execute(super::super::COMMAND_NET_INFO, &mut ram, 0..18, 64)
        .unwrap();
    ram[3] != 0
}

#[test]
fn failed_worker_recovers_after_a_failed_retry_and_discards_stale_frames() {
    let peer = Peer::new(true);
    let device = peer.device();
    device.port().receive(vec![42; 14]);
    peer.fail.store(true, Ordering::Relaxed);
    until(|| !device.state.lock().unwrap().link);
    assert!(
        device.host.ensure(&device).is_err(),
        "dead worker was cached as connected"
    );
    assert!(!link(&device), "guest must observe link-down first");
    until(|| {
        let _ = link(&device);
        peer.attempts.load(Ordering::Relaxed) >= 2
    });
    assert!(!link(&device), "failed retry must stay link-down");
    until(|| link(&device));
    let mut state = device.state.lock().unwrap();
    assert_eq!(peer.attempts.load(Ordering::Relaxed), 3);
    assert!(
        state.rx.is_empty(),
        "old attachment's packets leaked into the new link"
    );
    assert_eq!(state.counters.rx_dropped, 1);
    assert!(state.counters.backend_errors >= 2);
    state.tx.push_back(vec![99; 14]);
    drop(state);
    until(|| !device.state.lock().unwrap().rx.is_empty());
    assert_eq!(
        device.state.lock().unwrap().rx.pop_front().unwrap(),
        vec![99; 14]
    );
}

#[test]
fn heartbeat_timeout_recovers_without_a_runtime_restart() {
    let peer = Peer::new(false);
    let device = peer.device();
    peer.silent.store(true, Ordering::Relaxed);
    until(|| !device.state.lock().unwrap().link);
    assert!(!link(&device));
    peer.silent.store(false, Ordering::Relaxed);
    until(|| link(&device));
    assert_eq!(peer.attempts.load(Ordering::Relaxed), 2);
    assert_eq!(device.state.lock().unwrap().counters.backend_errors, 1);
}

#[test]
fn real_ethernet_progress_keeps_a_congested_heartbeat_channel_alive() {
    let peer = Peer::new(false);
    let device = peer.device();
    peer.silent.store(true, Ordering::Relaxed);
    let start = Instant::now();
    while start.elapsed() < Duration::from_secs(6) {
        device.state.lock().unwrap().tx.push_back(vec![42; 60]);
        thread::sleep(Duration::from_millis(100));
        assert!(device.state.lock().unwrap().link);
    }
    assert_eq!(peer.attempts.load(Ordering::Relaxed), 1);
    assert_eq!(device.state.lock().unwrap().counters.backend_errors, 0);
}

#[test]
fn slow_recovery_never_blocks_info_and_drop_reaps_temporary_sockets() {
    let peer = Peer::new(false);
    let device = peer.device();
    peer.withhold_hello.store(true, Ordering::Relaxed);
    peer.fail.store(true, Ordering::Relaxed);
    until(|| !device.state.lock().unwrap().link);
    assert!(!link(&device));
    until(|| {
        assert!(!link(&device));
        peer.attempts.load(Ordering::Relaxed) == 2
    });
    let start = Instant::now();
    for _ in 0..100 {
        assert!(!link(&device));
    }
    assert!(
        start.elapsed() < Duration::from_millis(100),
        "INFO waited on a broken helper"
    );
    drop(device);
    for address in peer.addresses.lock().unwrap().iter() {
        assert!(
            !address.parent().unwrap().exists(),
            "recovery leaked a private socket directory"
        );
    }
}
