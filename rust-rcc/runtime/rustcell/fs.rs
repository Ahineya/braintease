//! Owned descriptors over the guest filesystem, including mounted filesystems.
use crate::{Error, Result, ffi, io, result};
use alloc::vec::Vec;

pub struct File {
    fd: i16,
}
#[derive(Clone, Copy)]
pub struct OpenOptions {
    flags: u16,
}
pub enum SeekFrom {
    Start(u32),
    Current(i32),
    End(i32),
}

impl OpenOptions {
    pub const fn new() -> Self {
        Self { flags: 0 }
    }
    fn set(mut self, bit: u16, enabled: bool) -> Self {
        if enabled {
            self.flags |= bit;
        } else {
            self.flags &= !bit;
        }
        self
    }
    pub fn read(self, enabled: bool) -> Self {
        self.set(1, enabled)
    }
    pub fn write(self, enabled: bool) -> Self {
        self.set(2, enabled)
    }
    pub fn create(self, enabled: bool) -> Self {
        self.set(4, enabled)
    }
    pub fn truncate(self, enabled: bool) -> Self {
        self.set(8, enabled)
    }
    pub fn append(self, enabled: bool) -> Self {
        self.set(16, enabled)
    }
    pub fn create_new(self, enabled: bool) -> Self {
        self.set(32, enabled)
    }
    pub fn open(&self, path: &[u8]) -> Result<File> {
        let flags = if self.flags & 16 != 0 {
            self.flags | 2
        } else {
            self.flags
        };
        let fd = result(unsafe { ffi::rustcell_open(path.as_ptr(), path.len(), flags) })?;
        if fd > i16::MAX as usize {
            return Err(Error::InvalidData);
        }
        Ok(File { fd: fd as i16 })
    }
}

impl File {
    pub fn open(path: &[u8]) -> Result<Self> {
        OpenOptions::new().read(true).open(path)
    }
    pub fn create(path: &[u8]) -> Result<Self> {
        OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(path)
    }
    pub fn read(&mut self, buffer: &mut [u8]) -> Result<usize> {
        io::read(self.fd, buffer)
    }
    pub fn write(&mut self, bytes: &[u8]) -> Result<usize> {
        io::write(self.fd, bytes)
    }
    pub fn write_all(&mut self, bytes: &[u8]) -> Result<()> {
        io::write_all(self.fd, bytes)
    }
    pub fn read_exact(&mut self, bytes: &mut [u8]) -> Result<()> {
        let mut done = 0;
        while done < bytes.len() {
            let count = self.read(&mut bytes[done..])?;
            if count == 0 {
                return Err(Error::UnexpectedEof);
            }
            done += count;
        }
        Ok(())
    }
    pub fn read_to_end(&mut self, bytes: &mut Vec<u8>) -> Result<usize> {
        let original = bytes.len();
        let mut buffer = [0u8; 256];
        loop {
            let count = self.read(&mut buffer)?;
            if count == 0 {
                return Ok(bytes.len() - original);
            }
            bytes.extend_from_slice(&buffer[..count]);
        }
    }
    pub fn seek(&mut self, from: SeekFrom) -> Result<u32> {
        let (offset, origin) = match from {
            SeekFrom::Start(value) => {
                if value > i32::MAX as u32 {
                    return Err(Error::InvalidInput);
                }
                (value as i32, 0)
            }
            SeekFrom::Current(value) => (value, 1),
            SeekFrom::End(value) => (value, 2),
        };
        Ok(result(unsafe { ffi::rustcell_seek(self.fd, offset, origin) })? as u32)
    }
    pub fn close(mut self) -> Result<()> {
        let fd = self.fd;
        self.fd = -1;
        result(unsafe { ffi::rustcell_close(fd) })?;
        Ok(())
    }
}
impl Drop for File {
    fn drop(&mut self) {
        if self.fd >= 0 {
            unsafe {
                ffi::rustcell_close(self.fd);
            }
        }
    }
}

pub fn read(path: &[u8]) -> Result<Vec<u8>> {
    let mut bytes = Vec::new();
    File::open(path)?.read_to_end(&mut bytes)?;
    Ok(bytes)
}
pub fn write(path: &[u8], bytes: &[u8]) -> Result<()> {
    let mut file = File::create(path)?;
    file.write_all(bytes)?;
    file.close()
}
pub fn remove_file(path: &[u8]) -> Result<()> {
    result(unsafe { ffi::rustcell_unlink(path.as_ptr(), path.len()) })?;
    Ok(())
}
