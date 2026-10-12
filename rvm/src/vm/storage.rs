use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Write, Seek, SeekFrom};
use std::path::PathBuf;

const BLOCK_SIZE_WORDS: usize = 32768;  // 32K words per block
const BLOCK_SIZE_BYTES: usize = BLOCK_SIZE_WORDS * 2;  // 64KB per block
#[allow(dead_code)]
const MAX_BLOCKS: usize = 65536;  // 64K blocks total
#[allow(dead_code)]
const TOTAL_CAPACITY_BYTES: usize = MAX_BLOCKS * BLOCK_SIZE_BYTES;  // 4 GiB total

/// Storage subsystem for the Ripple VM
/// Provides persistent block storage with lazy initialization
pub struct Storage {
    /// Current selected block number (0-65535)
    current_block: u16,
    
    /// Current word address; 0..32767 maps this block and larger values read zero.
    current_addr: u16,
    
    /// Cached blocks in memory (lazy-loaded)
    /// Only blocks that have been accessed are loaded
    blocks: HashMap<u16, Block>,
    
    /// Backing file for persistent storage
    backing_file: Option<File>,
    
    /// Path to the backing file
    #[allow(dead_code)]
    backing_path: PathBuf,
    
    /// Busy flag for operations
    busy: bool,
}

/// A single block of storage
struct Block {
    /// Block data (32768 words)
    data: Vec<u16>,
    
    /// Whether this block has uncommitted writes
    dirty: bool,
}

impl Block {
    fn new() -> Self {
        Block {
            data: vec![0; BLOCK_SIZE_WORDS],
            dirty: false,
        }
    }
    
    fn from_bytes(bytes: &[u8]) -> Self {
        let mut data = Vec::with_capacity(BLOCK_SIZE_WORDS);
        for i in (0..bytes.len()).step_by(2) {
            let word = if i + 1 < bytes.len() {
                u16::from_le_bytes([bytes[i], bytes[i + 1]])
            } else {
                u16::from_le_bytes([bytes[i], 0])
            };
            data.push(word);
        }
        // Pad with zeros if needed
        while data.len() < BLOCK_SIZE_WORDS {
            data.push(0);
        }
        Block {
            data,
            dirty: false,
        }
    }
    
    fn to_bytes(&self) -> Vec<u8> {
        let mut bytes = Vec::with_capacity(BLOCK_SIZE_BYTES);
        for &word in &self.data {
            bytes.extend_from_slice(&word.to_le_bytes());
        }
        bytes
    }
}

impl Storage {
    /// Create a new storage subsystem with default path
    pub fn new() -> io::Result<Self> {
        // Determine the backing file path
        let home_dir = std::env::var("HOME")
            .or_else(|_| std::env::var("USERPROFILE"))  // Windows fallback
            .map_err(|_| io::Error::new(io::ErrorKind::NotFound, "Could not find home directory"))?;
        
        let mut backing_path = PathBuf::from(home_dir);
        backing_path.push(".RippleVM");
        
        // Create the directory if it doesn't exist
        std::fs::create_dir_all(&backing_path)?;
        
        backing_path.push("disk.img");
        
        Self::with_path(backing_path)
    }
    
    /// Create a new storage subsystem with a custom path
    pub fn with_path(backing_path: PathBuf) -> io::Result<Self> {
        // Create parent directory if it doesn't exist
        if let Some(parent) = backing_path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        
        // Open or create the backing file
        let backing_file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .open(&backing_path)?;
        
        Ok(Storage {
            current_block: 0,
            current_addr: 0,
            blocks: HashMap::new(),
            backing_file: Some(backing_file),
            backing_path,
            busy: false,
        })
    }
    
    /// Load a block from the backing file if not already cached
    fn load_block(&mut self, block_num: u16) -> io::Result<()> {
        if self.blocks.contains_key(&block_num) {
            return Ok(());  // Already loaded
        }
        
        log::debug!("Storage: Loading block {:#06x} from backing file", block_num);
        
        if let Some(ref mut file) = self.backing_file {
            let offset = block_num as u64 * BLOCK_SIZE_BYTES as u64;
            log::trace!("Storage: Seeking to offset {:#x} ({})", offset, offset);
            file.seek(SeekFrom::Start(offset))?;
            
            let mut buffer = vec![0u8; BLOCK_SIZE_BYTES];
            let mut filled = 0;
            while filled < buffer.len() {
                match file.read(&mut buffer[filled..]) {
                    Ok(0) => break,
                    Ok(count) => filled += count,
                    Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                    Err(error) => return Err(error),
                }
            }
            // A short final block retains its existing octets; only the tail
            // beyond EOF is zero. In particular, odd file sizes retain the low
            // octet of their final word.
            self.blocks.insert(block_num, Block::from_bytes(&buffer));
        } else {
            // No backing file, create empty block
            log::debug!("Storage: No backing file, creating empty block {:#06x}", block_num);
            self.blocks.insert(block_num, Block::new());
        }
        
        Ok(())
    }
    
    /// Commit a specific block to disk
    fn commit_block(&mut self, block_num: u16) -> io::Result<()> {
        if let Some(block) = self.blocks.get_mut(&block_num) {
            if !block.dirty {
                return Ok(());  // Nothing to commit
            }
            
            if let Some(ref mut file) = self.backing_file {
                let offset = block_num as u64 * BLOCK_SIZE_BYTES as u64;
                file.seek(SeekFrom::Start(offset))?;
                file.write_all(&block.to_bytes())?;
                file.flush()?;
                block.dirty = false;
            }
        }
        
        Ok(())
    }
    
    /// Set the current block number
    pub fn set_block(&mut self, block_num: u16) {
        self.current_block = block_num;
    }
    
    /// Set the current word address within the block
    pub fn set_addr(&mut self, addr: u16) {
        self.current_addr = addr;
    }
    
    /// Get the current block number
    pub fn get_block(&self) -> u16 {
        self.current_block
    }
    
    /// Get the current word address within the block
    pub fn get_addr(&self) -> u16 {
        self.current_addr
    }
    
    /// Read a word at the current (block, addr)
    pub fn read_word(&mut self) -> u16 {
        self.read_word_checked().unwrap_or_else(|error| {
            log::error!("Storage: read failed: {error}");
            0
        })
    }

    pub fn read_word_checked(&mut self) -> io::Result<u16> {
        self.load_block(self.current_block)?;

        let value = self.blocks
            .get(&self.current_block)
            .map(|block| {
                let word_idx = self.current_addr as usize;
                if word_idx < block.data.len() {
                    block.data[word_idx]
                } else {
                    0
                }
            })
            .unwrap_or(0);

        self.current_addr = self.current_addr.wrapping_add(1);
        Ok(value)
    }

    /// Write a word at the current (block, addr)
    pub fn write_word(&mut self, value: u16) {
        if let Err(error) = self.write_word_checked(value) {
            log::error!("Storage: write failed: {error}");
        }
    }

    pub fn write_word_checked(&mut self, value: u16) -> io::Result<()> {
        self.load_block(self.current_block)?;

        if let Some(block) = self.blocks.get_mut(&self.current_block) {
            let word_idx = self.current_addr as usize;
            if word_idx < block.data.len() {
                block.data[word_idx] = value;
                block.dirty = true;
            }
        }

        self.current_addr = self.current_addr.wrapping_add(1);
        Ok(())
    }

    /// Get the control register value
    pub fn get_control(&self) -> u16 {
        let mut control = 0u16;
        
        // Bit 0: BUSY
        if self.busy {
            control |= 1 << 0;
        }
        
        // Bit 1: DIRTY (current block)
        if let Some(block) = self.blocks.get(&self.current_block) {
            if block.dirty {
                control |= 1 << 1;
            }
        }
        
        control
    }
    
    /// Handle writes to the control register
    pub fn set_control(&mut self, value: u16) {
        if let Err(error) = self.set_control_checked(value) {
            log::error!("Storage: control failed: {error}");
        }
    }

    pub fn set_control_checked(&mut self, value: u16) -> io::Result<()> {
        // Bit 2: COMMIT (current block)
        if value & (1 << 2) != 0 {
            self.busy = true;
            let result = self.commit_block(self.current_block);
            self.busy = false;
            result?;
        }
        
        // Bit 3: COMMIT_ALL
        if value & (1 << 3) != 0 {
            self.busy = true;
            let result = self.flush_checked();
            self.busy = false;
            result?;
        }
        Ok(())
    }
    
    /// Commit all dirty blocks to disk
    pub fn commit_all(&mut self) {
        if let Err(error) = self.flush_checked() {
            log::error!("Storage: flush failed: {error}");
        }
    }

    pub fn flush_checked(&mut self) -> io::Result<()> {
        let dirty_blocks: Vec<u16> = self.blocks
            .iter()
            .filter(|(_, block)| block.dirty)
            .map(|(&num, _)| num)
            .collect();
        
        for block_num in dirty_blocks {
            self.commit_block(block_num)?;
        }
        Ok(())
    }
    
    /// Flush all changes to disk (called on shutdown)
    pub fn flush(&mut self) {
        self.commit_all();
    }
}

impl Drop for Storage {
    fn drop(&mut self) {
        // Ensure all changes are flushed when storage is dropped
        self.flush();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};
    static NEXT_FILE: AtomicU64 = AtomicU64::new(0);

    struct Disk(PathBuf);
    impl Disk {
        fn new(bytes: &[u8]) -> Self {
            let path = std::env::temp_dir().join(format!("rvm-storage-{}-{}.img",
                std::process::id(), NEXT_FILE.fetch_add(1, Ordering::Relaxed)));
            std::fs::write(&path, bytes).unwrap();
            Self(path)
        }
    }
    impl Drop for Disk {
        fn drop(&mut self) { let _ = std::fs::remove_file(&self.0); }
    }

    #[test]
    fn partial_final_blocks_retain_existing_octets_and_zero_pad_tail() {
        let disk = Disk::new(&[0x11, 0x22, 0x33]);
        let mut storage = Storage::with_path(disk.0.clone()).unwrap();
        assert_eq!(storage.read_word_checked().unwrap(), 0x2211);
        assert_eq!(storage.read_word_checked().unwrap(), 0x0033);
        assert_eq!(storage.read_word_checked().unwrap(), 0);
        storage.set_block(1);
        assert_eq!(storage.read_word_checked().unwrap(), 0);
        assert_eq!(std::fs::read(&disk.0).unwrap(), [0x11, 0x22, 0x33]);
    }

    #[test]
    fn checked_storage_preserves_word_addresses_wrap_and_commit() {
        let disk = Disk::new(&[]);
        {
            let mut storage = Storage::with_path(disk.0.clone()).unwrap();
            storage.set_addr(32767);
            storage.write_word_checked(0xabcd).unwrap();
            assert_eq!(storage.get_addr(), 32768);
            assert_eq!(storage.get_control(), 2);
            storage.write_word_checked(0xffff).unwrap(); // Outside this block's words.
            storage.set_addr(u16::MAX);
            assert_eq!(storage.read_word_checked().unwrap(), 0);
            assert_eq!(storage.get_addr(), 0);
            assert_eq!(storage.get_block(), 0);
            storage.set_control_checked(4).unwrap();
            assert_eq!(storage.get_control(), 0);
        }
        let data = std::fs::read(&disk.0).unwrap();
        assert_eq!(data.len(), 65536);
        assert_eq!(&data[65534..], &[0xcd, 0xab]);
    }

    #[test]
    fn checked_io_reports_failures_without_advancing_or_losing_dirty_state() {
        let disk = Disk::new(&[]);
        let mut storage = Storage::with_path(disk.0.clone()).unwrap();
        storage.backing_file = Some(OpenOptions::new().write(true).open(&disk.0).unwrap());
        assert!(storage.read_word_checked().is_err());
        assert_eq!(storage.get_addr(), 0);
        assert!(storage.write_word_checked(42).is_err());
        assert_eq!(storage.get_addr(), 0);
        storage.backing_file = Some(OpenOptions::new().read(true).open(&disk.0).unwrap());
        storage.write_word_checked(42).unwrap();
        assert!(storage.set_control_checked(4).is_err());
        assert_eq!(storage.get_control(), 2); // Not busy; still dirty.
        assert!(storage.flush_checked().is_err());
        // Restore a writable handle so cleanup also checks successful retry.
        storage.backing_file = Some(OpenOptions::new().read(true).write(true).open(&disk.0).unwrap());
        storage.flush_checked().unwrap();
        assert_eq!(storage.get_control(), 0);
    }
}
