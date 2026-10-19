//! Bounded, memory-backed browser disk. It never opens a host pathname.
use std::{collections::BTreeSet, io, path::PathBuf};
const BLOCK: usize = 65536;
const MAX: usize = 256 * 1024 * 1024;
pub struct Storage {
    pub(crate) bytes: Vec<u8>,
    block: u16,
    addr: u16,
    dirty: BTreeSet<u16>,
    error: bool,
}
#[derive(Clone, Debug, serde::Serialize, serde::Deserialize)]
pub struct StorageCheckpoint;
impl Storage {
    pub fn new() -> io::Result<Self> {
        Ok(Self {
            bytes: Vec::new(),
            block: 0,
            addr: 0,
            dirty: BTreeSet::new(),
            error: false,
        })
    }
    pub fn with_path(_path: PathBuf) -> io::Result<Self> {
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "browser disks require explicit image bytes",
        ))
    }
    pub fn from_bytes(bytes: Vec<u8>) -> Result<Self, String> {
        if bytes.is_empty() || bytes.len() > MAX {
            return Err("browser disk must be 1..256 MiB".into());
        }
        let mut s = Self::new().map_err(|e| e.to_string())?;
        s.bytes = bytes;
        Ok(s)
    }
    pub fn set_block(&mut self, v: u16) {
        self.block = v;
    }
    pub fn set_addr(&mut self, v: u16) {
        self.addr = v;
    }
    pub fn get_block(&self) -> u16 {
        self.block
    }
    pub fn get_addr(&self) -> u16 {
        self.addr
    }
    pub fn read_word(&mut self) -> u16 {
        let at = self.block as usize * BLOCK + self.addr as usize * 2;
        let v = if self.addr < 32768 {
            u16::from_le_bytes([
                *self.bytes.get(at).unwrap_or(&0),
                *self.bytes.get(at + 1).unwrap_or(&0),
            ])
        } else {
            0
        };
        self.addr = self.addr.wrapping_add(1);
        v
    }
    pub fn write_word(&mut self, v: u16) {
        if self.addr < 32768 {
            let at = self.block as usize * BLOCK + self.addr as usize * 2;
            if let Some(out) = self.bytes.get_mut(at..at + 2) {
                out.copy_from_slice(&v.to_le_bytes());
                self.dirty.insert(self.block);
            } else {
                self.error = true;
            }
        }
        self.addr = self.addr.wrapping_add(1);
    }
    pub fn read_bytes_at(&mut self, offset: u32, count: usize) -> io::Result<Vec<u8>> {
        let at = offset as usize;
        let end = at
            .checked_add(count)
            .filter(|v| *v <= MAX)
            .ok_or_else(|| io::Error::from(io::ErrorKind::InvalidInput))?;
        let mut out = vec![0; count];
        if at < self.bytes.len() {
            let n = end.min(self.bytes.len()) - at;
            out[..n].copy_from_slice(&self.bytes[at..at + n]);
        }
        Ok(out)
    }
    pub fn write_bytes_at(&mut self, offset: u32, bytes: &[u8]) -> io::Result<()> {
        let at = offset as usize;
        let end = at
            .checked_add(bytes.len())
            .filter(|v| *v <= self.bytes.len())
            .ok_or_else(|| io::Error::from(io::ErrorKind::UnexpectedEof))?;
        self.bytes[at..end].copy_from_slice(bytes);
        if !bytes.is_empty() {
            for b in at / BLOCK..=(end - 1) / BLOCK {
                self.dirty.insert(b as u16);
            }
        }
        Ok(())
    }
    pub fn get_control(&self) -> u16 {
        u16::from(self.dirty.contains(&self.block)) * 2 + u16::from(self.error) * 16
    }
    pub fn set_control(&mut self, v: u16) {
        if v & 16 != 0 {
            self.error = false;
        }
        if v & 8 != 0 {
            self.dirty.clear();
        } else if v & 4 != 0 {
            self.dirty.remove(&self.block);
        }
    }
    pub fn flush(&mut self) {
        self.dirty.clear();
    }
    pub fn flush_checked(&mut self) -> io::Result<()> {
        self.flush();
        Ok(())
    }
}
