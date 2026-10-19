//! Lazy host attachment. Device snapshots contain no process or socket handles.
use super::{NetworkBackend, NetworkDevice};
use std::{
    ffi::OsStr,
    path::PathBuf,
    sync::{Arc, Mutex, OnceLock},
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

#[derive(Debug, Default, Clone, PartialEq, Eq)]
enum Configuration {
    #[default]
    Off,
    Host,
    Shared,
    Socket(PathBuf),
}

#[derive(Default)]
pub(super) struct HostNetwork {
    configuration: Configuration,
    connection: OnceLock<Result<Mutex<Attachment>, String>>,
}
impl std::fmt::Debug for HostNetwork {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("HostNetwork")
            .field("configuration", &self.configuration)
            .field("initialized", &self.connection.get().is_some())
            .finish()
    }
}
// Drop order matters: detach the NIC before stopping its owned helper.
struct Connection {
    _backend: NetworkBackend,
    #[cfg(target_os = "macos")]
    _helper: Option<super::vmnet::ManagedHelper>,
}
// Initial authorization errors remain cached. Only an attachment that worked
// before is eligible for noninteractive recovery. Cleanup and reconnection run
// outside the emulation thread; a slow helper must not freeze the guest UI.
struct Attachment {
    connection: Option<Connection>,
    recovery: Option<JoinHandle<Result<Connection, String>>>,
    retry_at: Option<Instant>,
    delay: Duration,
}
impl Attachment {
    fn new(connection: Connection) -> Self {
        Self {
            connection: Some(connection),
            recovery: None,
            retry_at: None,
            delay: Duration::from_secs(1),
        }
    }
    fn ensure(
        &mut self,
        configuration: &Configuration,
        device: &NetworkDevice,
    ) -> Result<(), String> {
        if self
            .recovery
            .as_ref()
            .is_some_and(|worker| worker.is_finished())
        {
            match self.recovery.take().unwrap().join() {
                Ok(Ok(connection)) => {
                    self.connection = Some(connection);
                    self.retry_at = None;
                    self.delay = Duration::from_secs(1);
                    eprintln!("network: Ethernet attachment restored");
                }
                result => {
                    let error = match result {
                        Ok(Err(error)) => error,
                        Err(_) => "network recovery worker panicked".into(),
                        Ok(Ok(_)) => unreachable!(),
                    };
                    device.port().backend_failed();
                    self.failed(&error);
                }
            }
        }
        if self.recovery.is_some() {
            return Err("network attachment is recovering".into());
        }
        if self.connection.is_some() && device.state.lock().unwrap().link {
            return Ok(());
        }
        let now = Instant::now();
        let Some(retry_at) = self.retry_at else {
            // Return link-down to the guest before reconnecting, including
            // helpers which reuse the same MAC. This invalidates old TCP/ARP.
            self.retry_at = Some(now + self.delay);
            return Err("network attachment lost; recovery scheduled".into());
        };
        if now < retry_at {
            return Err("network attachment is waiting to retry".into());
        }
        let previous = self.connection.take();
        let configuration = configuration.clone();
        // This private alias shares only the bounded NIC queues, never guest
        // memory or the HostNetwork manager itself.
        let alias = NetworkDevice {
            state: Arc::clone(&device.state),
            host: HostNetwork::default(),
        };
        match thread::Builder::new()
            .name("ripple-net-recovery".into())
            .spawn(move || {
                drop(previous); // joins the old worker, then stops its owned helper
                {
                    let mut state = alias.state.lock().unwrap();
                    state.generation = state.generation.wrapping_add(1);
                    state.counters.rx_dropped = state
                        .counters
                        .rx_dropped
                        .saturating_add(state.rx.len() as u32);
                    state.rx.clear();
                }
                HostNetwork::open(&configuration, &alias, true)
            }) {
            Ok(worker) => self.recovery = Some(worker),
            Err(error) => self.failed(&format!("start network recovery worker: {error}")),
        }
        Err("network attachment is recovering".into())
    }
    fn failed(&mut self, error: &str) {
        eprintln!(
            "network recovery failed: {error}; retry in {}s",
            self.delay.as_secs()
        );
        self.retry_at = Some(Instant::now() + self.delay);
        self.delay = (self.delay * 2).min(Duration::from_secs(30));
    }
}
impl Drop for Attachment {
    fn drop(&mut self) {
        if let Some(worker) = self.recovery.take() {
            // Dropping a runtime must also reap a helper started during recovery.
            let _ = worker.join();
        }
    }
}
impl HostNetwork {
    pub(super) fn new(setting: Option<&OsStr>) -> Self {
        let setting = setting.unwrap_or(OsStr::new("auto"));
        let configuration = match setting.to_str() {
            Some("off") => Configuration::Off,
            Some("host") => Configuration::Host,
            Some("shared") => Configuration::Shared,
            Some("auto") => {
                if cfg!(target_os = "macos") {
                    Configuration::Host
                } else {
                    Configuration::Off
                }
            }
            _ => Configuration::Socket(PathBuf::from(setting)),
        };
        Self {
            configuration,
            connection: OnceLock::new(),
        }
    }
    pub(super) fn is_external(&self) -> bool {
        matches!(self.configuration, Configuration::Socket(_))
    }
    pub(super) fn ensure(&self, device: &NetworkDevice) -> Result<(), String> {
        if self.configuration == Configuration::Off {
            return Ok(());
        }
        self.connection
            .get_or_init(|| {
                let result = Self::open(&self.configuration, device, false);
                if let Err(error) = &result {
                    eprintln!("network initialization failed: {error}");
                    device.port().backend_failed();
                }
                result.map(|connection| Mutex::new(Attachment::new(connection)))
            })
            .as_ref()
            .map_err(Clone::clone)?
            .lock()
            .unwrap()
            .ensure(&self.configuration, device)
    }
    fn open(
        configuration: &Configuration,
        device: &NetworkDevice,
        recovering: bool,
    ) -> Result<Connection, String> {
        if let Configuration::Socket(path) = configuration {
            return NetworkBackend::connect(device, path).map(|backend| Connection {
                _backend: backend,
                #[cfg(target_os = "macos")]
                _helper: None,
            });
        }
        #[cfg(target_os = "macos")]
        {
            let mode = if *configuration == Configuration::Shared {
                "shared"
            } else {
                "host"
            };
            let helper = super::vmnet::ManagedHelper::start(mode, !recovering)?;
            let backend = NetworkBackend::connect(device, &helper.socket())?;
            eprintln!("network: {mode} Ethernet ready (host/gateway 192.168.64.1)");
            Ok(Connection {
                _backend: backend,
                _helper: Some(helper),
            })
        }
        #[cfg(not(target_os = "macos"))]
        {
            let _ = recovering;
            Err("automatic Ethernet attachment currently requires macOS; use an external socket backend or disable networking".into())
        }
    }
}

#[cfg(test)]
#[cfg(unix)]
#[path = "host_recovery_tests.rs"]
mod recovery_tests;

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn capabilities_clock_and_invalid_nic_queries_never_start_networking() {
        use crate::devices::*;
        let mut device = DeviceState::default();
        device
            .network
            .configure_host(Some(OsStr::new("host")))
            .unwrap();
        let mut ram = vec![0; 256];
        device.write_register(REGISTER_DESCRIPTOR_OFFSET, 64, &mut ram, 128, 0);
        for (command, length, interface, status) in [
            (COMMAND_CAPABILITIES, 4, 0, STATUS_OK),
            (COMMAND_GET_TIME, 8, 0, STATUS_OK),
            (super::super::COMMAND_NET_INFO, 18, 1, STATUS_BAD_DESCRIPTOR),
        ] {
            ram[64..64 + length].fill(0);
            ram[64] = 1;
            ram[65] = length as u16;
            ram[66] = interface;
            device.write_register(REGISTER_COMMAND, command, &mut ram, 128, 0);
            assert_eq!(device.status(), status);
            assert!(device.network.host.connection.get().is_none());
        }
    }
    #[test]
    fn configuration_is_lazy_and_off_never_connects() {
        let mut device = NetworkDevice::default();
        device.configure_host(Some(OsStr::new("off"))).unwrap();
        device.host.ensure(&device).unwrap();
        assert!(device.host.connection.get().is_none());
        device.configure_host(Some(OsStr::new("host"))).unwrap();
        assert!(device.host.connection.get().is_none());
        let snapshot = device.clone();
        assert_eq!(snapshot.host.configuration, Configuration::Off);
        assert!(serde_json::to_vec(&device).is_ok());
    }
    #[test]
    fn initialization_failure_is_cached_without_retrying_or_claiming_a_live_nic() {
        let mut device = NetworkDevice::default();
        assert!(device
            .configure_host(Some(OsStr::new("/no-such-ripple-net/socket")))
            .is_err());
        assert!(device.host.ensure(&device).is_err());
        let state = device.state.lock().unwrap();
        assert_eq!(state.counters.backend_errors, 1);
        assert!(!state.link && !state.live);
    }
}
