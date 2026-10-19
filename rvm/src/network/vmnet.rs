//! Owned macOS vmnet helper; its executable is embedded in the runtime.
use std::{
    fs::{self, File},
    io::{BufRead, BufReader, Write},
    os::unix::fs::{DirBuilderExt, OpenOptionsExt},
    path::PathBuf,
    process::{Child, Command, Stdio},
    sync::{
        atomic::{AtomicU64, Ordering},
        mpsc,
    },
    time::{Duration, Instant},
};

static HELPER: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/ripple-net-vmnet"));
pub(super) struct ManagedHelper {
    child: Option<Child>,
    directory: PathBuf,
    log_worker: Option<std::thread::JoinHandle<()>>,
}
impl ManagedHelper {
    pub(super) fn socket(&self) -> PathBuf {
        self.directory.join("nic.sock")
    }
    pub(super) fn start(mode: &str, authorize: bool) -> Result<Self, String> {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        // /tmp keeps Unix socket paths short, even with a long macOS TMPDIR.
        let directory = PathBuf::from(format!(
            "/tmp/ripple-vmnet-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::DirBuilder::new()
            .mode(0o700)
            .create(&directory)
            .map_err(|e| format!("private vmnet directory: {e}"))?;
        let mut owner = Self {
            child: None,
            directory,
            log_worker: None,
        };
        let executable = owner.directory.join("helper");
        File::options()
            .write(true)
            .create_new(true)
            .mode(0o700)
            .open(&executable)
            .and_then(|mut file| file.write_all(HELPER))
            .map_err(|e| format!("extract embedded vmnet helper: {e}"))?;
        eprintln!(
            "network: starting macOS {mode} Ethernet; only the helper requires sudo authorization"
        );
        if authorize {
            let authorized = Command::new("/usr/bin/sudo")
                .arg("-v")
                .status()
                .map_err(|e| format!("vmnet authorization: {e}"))?;
            if !authorized.success() {
                return Err("macOS vmnet authorization was declined; use --net off or RCC_NATIVE_NET=off for an offline run".into());
            }
        }
        let mut log =
            File::create(owner.directory.join("helper.log")).map_err(|e| e.to_string())?;
        let mut child = Command::new("/usr/bin/sudo")
            .args(["-n", "--"])
            .arg(executable)
            .arg(owner.socket())
            .arg(mode)
            .arg("--managed")
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|e| format!("start vmnet helper: {e}"))?;
        let stdout = child.stdout.take().unwrap();
        let stderr = child.stderr.take().unwrap();
        owner.child = Some(child);
        // Keep the failure reason in the session log before recovery removes
        // this private directory. Previously only ECONNRESET reached users,
        // while the actual vmnet/sendto error was silently deleted on Drop.
        owner.log_worker = Some(std::thread::spawn(move || {
            for line in BufReader::new(stderr).lines() {
                let line = match line {
                    Ok(line) => line,
                    Err(error) => {
                        eprintln!("network helper log: {error}");
                        break;
                    }
                };
                if let Err(error) = writeln!(log, "{line}") {
                    eprintln!("network helper log write: {error}");
                }
                if !line.starts_with("vmnet frames:") && !line.contains(" ready: socket=") {
                    eprintln!("network helper: {line}");
                }
            }
        }));
        let (sender, receiver) = mpsc::channel();
        std::thread::spawn(move || {
            let mut line = String::new();
            let result = BufReader::new(stdout).read_line(&mut line).map(|_| line);
            let _ = sender.send(result);
        });
        let ready = receiver.recv_timeout(Duration::from_secs(15));
        if !matches!(ready, Ok(Ok(ref line)) if line == "RNET READY\n") {
            let log = fs::read_to_string(owner.directory.join("helper.log")).unwrap_or_default();
            return Err(format!(
                "vmnet helper failed to become ready: {ready:?}\n{log}"
            ));
        }
        Ok(owner)
    }
}
impl Drop for ManagedHelper {
    fn drop(&mut self) {
        let mut reaped = true;
        if let Some(child) = &mut self.child {
            // The sudo parent retains our real UID and forwards TERM. No
            // second sudo authorization is needed when a long session ends.
            if matches!(child.try_wait(), Ok(None)) {
                let _ = Command::new("/bin/kill")
                    .args(["-TERM", &child.id().to_string()])
                    .status();
            }
            let deadline = Instant::now() + Duration::from_secs(6);
            while matches!(child.try_wait(), Ok(None)) && Instant::now() < deadline {
                std::thread::sleep(Duration::from_millis(10));
            }
            match child.try_wait() {
                Ok(Some(status)) => {
                    if !status.success() {
                        eprintln!("network helper exited: {status}");
                    }
                }
                result => {
                    eprintln!(
                        "network: helper did not stop; keeping {} ({result:?})",
                        self.directory.display()
                    );
                    reaped = false;
                }
            }
        }
        if !reaped {
            return;
        }
        if let Some(worker) = self.log_worker.take() {
            let _ = worker.join();
        }
        // A fatal helper exit (including perror/exit paths) can leave its
        // socket behind. Only unlink it after sudo has reaped the child.
        for name in ["nic.sock", "helper", "helper.log"] {
            let _ = fs::remove_file(self.directory.join(name));
        }
        if let Err(error) = fs::remove_dir(&self.directory) {
            if error.kind() == std::io::ErrorKind::NotFound {
                return;
            }
            eprintln!(
                "network: vmnet directory {} remains: {error}",
                self.directory.display()
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::net::UnixDatagram;

    #[test]
    fn exited_helper_with_a_leftover_socket_is_reaped_and_cleaned() {
        let directory = std::env::temp_dir().join(format!("vmnet-reap-{}", std::process::id()));
        fs::create_dir(&directory).unwrap();
        let socket = UnixDatagram::bind(directory.join("nic.sock")).unwrap();
        fs::write(
            directory.join("helper.log"),
            "vmnet_write failed (status=1001)\n",
        )
        .unwrap();
        let mut child = Command::new("/bin/sh")
            .args(["-c", "exit 7"])
            .spawn()
            .unwrap();
        child.wait().unwrap();
        drop(ManagedHelper {
            child: Some(child),
            directory: directory.clone(),
            log_worker: None,
        });
        drop(socket);
        assert!(
            !directory.exists(),
            "fatal helper exit leaked its socket directory"
        );
    }
}
