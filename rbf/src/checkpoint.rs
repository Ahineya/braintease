//! Portable execution checkpoints. The BF image and mounted disk remain explicit
//! inputs; both must match. No guest instruction is decoded or executed here.
use crate::{
    devices::{BfDevices, BfDevicesSnapshot},
    image::Layout,
    runner::Host,
};
use bf_wasm::engine::{Machine, Program};
use rvm::{devices::KeyTransition, runner::Controller, vm::Storage};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs::{self, File, OpenOptions},
    io::{self, BufReader, BufWriter, Read, Seek, SeekFrom, Write},
    path::Path,
    sync::atomic::{AtomicU64, Ordering},
};

const MAGIC: &[u8; 8] = b"RBFSTATE";
const VERSION: u32 = 1;
const MAX_METADATA: u64 = 64 * 1024 * 1024;
static NEXT_FILE: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Provenance {
    pub source_sha256: String,
    pub optimized: bool,
    pub seed: u32,
    pub input_script_sha256: Option<String>,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct State {
    provenance: Provenance,
    layout: Layout,
    instructions: u64,
    controller_credits: u64,
    controller_events: Vec<(u16, bool)>,
    devices: BfDevicesSnapshot,
}

struct HashedWriter<W> {
    inner: W,
    hash: Sha256,
}
impl<W: Write> Write for HashedWriter<W> {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let count = self.inner.write(bytes)?;
        self.hash.update(&bytes[..count]);
        Ok(count)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.inner.flush()
    }
}

impl Host {
    /// Commit a complete checkpoint using a sibling temporary file and atomic
    /// rename. A failed write never replaces a previous successful checkpoint.
    pub fn save_checkpoint(
        &mut self,
        path: &Path,
        program: &Program,
        provenance: &Provenance,
    ) -> Result<(), String> {
        let parent = path
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or(Path::new("."));
        fs::create_dir_all(parent).map_err(|e| format!("Checkpoint directory: {e}"))?;
        let name = path
            .file_name()
            .ok_or("Checkpoint needs a filename")?
            .to_string_lossy();
        let temp = parent.join(format!(
            ".{name}.{}-{}.tmp",
            std::process::id(),
            NEXT_FILE.fetch_add(1, Ordering::Relaxed)
        ));
        let file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temp)
            .map_err(|e| format!("Creating checkpoint {}: {e}", path.display()))?;
        let result = (|| -> Result<(), String> {
            let state = State {
                provenance: provenance.clone(),
                layout: self.layout.clone(),
                instructions: self.instructions,
                controller_credits: self.controller.credits,
                controller_events: self
                    .controller
                    .events
                    .iter()
                    .map(|e| (e.key, e.down))
                    .collect(),
                devices: self.devices.checkpoint()?,
            };
            let metadata = serde_json::to_vec(&state).map_err(|e| e.to_string())?;
            if metadata.len() as u64 > MAX_METADATA {
                return Err("Checkpoint device metadata exceeds 64 MiB".into());
            }
            let mut writer = HashedWriter {
                inner: BufWriter::new(file),
                hash: Sha256::new(),
            };
            writer.write_all(MAGIC).map_err(|e| e.to_string())?;
            writer
                .write_all(&VERSION.to_le_bytes())
                .map_err(|e| e.to_string())?;
            writer
                .write_all(&(metadata.len() as u64).to_le_bytes())
                .map_err(|e| e.to_string())?;
            writer.write_all(&metadata).map_err(|e| e.to_string())?;
            self.machine
                .write_checkpoint(program, &mut writer)
                .map_err(|e| e.to_string())?;
            let checksum = writer.hash.finalize();
            writer
                .inner
                .write_all(&checksum)
                .map_err(|e| e.to_string())?;
            writer.inner.flush().map_err(|e| e.to_string())?;
            writer
                .inner
                .get_ref()
                .sync_all()
                .map_err(|e| e.to_string())?;
            fs::rename(&temp, path).map_err(|e| format!("Committing checkpoint: {e}"))?;
            Ok(())
        })();
        if result.is_err() {
            let _ = fs::remove_file(&temp);
        }
        result.map_err(|e| format!("Saving checkpoint {}: {e}", path.display()))
    }

    pub fn load_checkpoint(
        path: &Path,
        program: &Program,
        layout: &Layout,
        expected_source_sha256: &str,
        optimized: bool,
        storage: Option<Storage>,
    ) -> Result<(Self, Provenance), String> {
        let read = || -> Result<(Self, Provenance), String> {
            let file = File::open(path).map_err(|e| e.to_string())?;
            let length = file.metadata().map_err(|e| e.to_string())?.len();
            if length < 52 {
                return Err("Truncated checkpoint".into());
            }
            // Verify the entire file before interpreting any saved state.
            let mut reader = BufReader::new(file);
            let mut hash = Sha256::new();
            let mut remaining = length - 32;
            let mut buffer = [0; 65536];
            while remaining != 0 {
                let count = remaining.min(buffer.len() as u64) as usize;
                reader
                    .read_exact(&mut buffer[..count])
                    .map_err(|e| e.to_string())?;
                hash.update(&buffer[..count]);
                remaining -= count as u64;
            }
            let mut checksum = [0; 32];
            reader
                .read_exact(&mut checksum)
                .map_err(|e| e.to_string())?;
            let actual: [u8; 32] = hash.finalize().into();
            if actual != checksum {
                return Err("Checkpoint checksum mismatch".into());
            }
            reader.seek(SeekFrom::Start(0)).map_err(|e| e.to_string())?;
            let mut body = reader.take(length - 32);
            let mut magic = [0; 8];
            body.read_exact(&mut magic).map_err(|e| e.to_string())?;
            if &magic != MAGIC {
                return Err("Not an RBF checkpoint".into());
            }
            let mut version = [0; 4];
            body.read_exact(&mut version).map_err(|e| e.to_string())?;
            if u32::from_le_bytes(version) != VERSION {
                return Err("Unsupported RBF checkpoint version".into());
            }
            let mut size = [0; 8];
            body.read_exact(&mut size).map_err(|e| e.to_string())?;
            let size = u64::from_le_bytes(size);
            if size > MAX_METADATA || size > body.limit() {
                return Err("Invalid checkpoint metadata size".into());
            }
            let mut metadata = vec![0; size as usize];
            body.read_exact(&mut metadata).map_err(|e| e.to_string())?;
            let state: State = serde_json::from_slice(&metadata)
                .map_err(|e| format!("Invalid checkpoint metadata: {e}"))?;
            if state.provenance.source_sha256 != expected_source_sha256 {
                return Err("Checkpoint belongs to a different BF program; use the unchanged image that created it".into());
            }
            if state.provenance.optimized != optimized {
                return Err(
                    "Checkpoint optimization mode differs; use the same --no-optimize setting"
                        .into(),
                );
            }
            if &state.layout != layout {
                return Err("Checkpoint BFHOST layout differs".into());
            }
            if state.devices.bank_size() != layout.bank_size {
                return Err("Checkpoint device bank size differs".into());
            }
            layout.validate(layout.tape_cells)?;
            if state.controller_events.len() > 4096
                || state.controller_events.iter().any(|(key, _)| *key >= 256)
            {
                return Err("Invalid checkpoint controller input".into());
            }
            let machine = Machine::read_checkpoint(program, &mut body, layout.tape_cells)
                .map_err(|e| e.to_string())?;
            if machine.tape_len() != layout.tape_cells {
                return Err("Checkpoint tape size differs from BFHOST layout".into());
            }
            if body.limit() != 0 {
                return Err("Unexpected trailing checkpoint data".into());
            }
            let devices = BfDevices::restore_checkpoint(state.devices, storage)?;
            let host = Self {
                layout: layout.clone(),
                machine,
                devices,
                instructions: state.instructions,
                profile: None,
                controller: Controller {
                    credits: state.controller_credits,
                    events: state
                        .controller_events
                        .into_iter()
                        .map(|(key, down)| KeyTransition { key, down })
                        .collect(),
                    // Connections, cancellation and window closure are host lifecycle
                    // state. Reopening a saved session must not immediately stop it.
                    closed: false,
                    quit: false,
                },
            };
            Ok((host, state.provenance))
        };
        read().map_err(|e| format!("Loading checkpoint {}: {e}", path.display()))
    }
}
