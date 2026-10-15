//! Lazy host attachment. Device snapshots contain no process or socket handles.
use super::{NetworkBackend, NetworkDevice};
use std::{ffi::OsStr, path::PathBuf, sync::OnceLock};

#[derive(Debug, Default, PartialEq, Eq)]
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
    connection: OnceLock<Result<Connection, String>>,
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
    pub(super) fn ensure(&self, device: &NetworkDevice) -> Result<(), &str> {
        if self.configuration == Configuration::Off {
            return Ok(());
        }
        self.connection
            .get_or_init(|| {
                let result = self.open(device);
                if let Err(error) = &result {
                    eprintln!("network initialization failed: {error}");
                    device.port().backend_failed();
                }
                result
            })
            .as_ref()
            .map(|_| ())
            .map_err(String::as_str)
    }
    fn open(&self, device: &NetworkDevice) -> Result<Connection, String> {
        if let Configuration::Socket(path) = &self.configuration {
            return NetworkBackend::connect(device, path).map(|backend| Connection {
                _backend: backend,
                #[cfg(target_os = "macos")]
                _helper: None,
            });
        }
        #[cfg(target_os = "macos")]
        {
            let mode = if self.configuration == Configuration::Shared {
                "shared"
            } else {
                "host"
            };
            let helper = super::vmnet::ManagedHelper::start(mode)?;
            let backend = NetworkBackend::connect(device, &helper.socket())?;
            eprintln!("network: {mode} Ethernet ready (host/gateway 192.168.64.1)");
            Ok(Connection {
                _backend: backend,
                _helper: Some(helper),
            })
        }
        #[cfg(not(target_os = "macos"))]
        Err("automatic Ethernet attachment currently requires macOS; use an external socket backend or disable networking".into())
    }
}

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
