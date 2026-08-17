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
    
    /// Current byte address within the block (0-65535)
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
            match file.read_exact(&mut buffer) {
                Ok(_) => {
                    // Successfully read the block
                    log::trace!("Storage: Successfully read block {:#06x}, first 16 bytes: {:02x?}", 
                                block_num, &buffer[0..16.min(buffer.len())]);
                    let block = Block::from_bytes(&buffer);
                    self.blocks.insert(block_num, block);
                }
                Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => {
                    // Block doesn't exist yet, create an empty one
                    log::debug!("Storage: Block {:#06x} doesn't exist in file (EOF), creating empty block", block_num);
                    self.blocks.insert(block_num, Block::new());
                }
                Err(e) => {
                    log::error!("Storage: Error reading block {:#06x}: {:?}", block_num, e);
                    return Err(e);
                }
            }
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
    
    /// Set the current byte address within the block
    pub fn set_addr(&mut self, addr: u16) {
        self.current_addr = addr;
    }
    
    /// Get the current block number
    pub fn get_block(&self) -> u16 {
        self.current_block
    }
    
    /// Get the current byte address within the block
    pub fn get_addr(&self) -> u16 {
        self.current_addr
    }
    
    /// Read a word at the current (block, addr)
    pub fn read_word(&mut self) -> u16 {
        if let Err(e) = self.load_block(self.current_block) {
            log::error!("Storage: Failed to load block {:#06x}: {:?}", self.current_block, e);
            return 0;
        }

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
        value
    }

    /// Write a word at the current (block, addr)
    pub fn write_word(&mut self, value: u16) {
        if self.load_block(self.current_block).is_err() {
            return;
        }

        if let Some(block) = self.blocks.get_mut(&self.current_block) {
            let word_idx = self.current_addr as usize;
            if word_idx < block.data.len() {
                block.data[word_idx] = value;
                block.dirty = true;
            }
        }

        self.current_addr = self.current_addr.wrapping_add(1);
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
        // Bit 2: COMMIT (current block)
        if value & (1 << 2) != 0 {
            self.busy = true;
            let _ = self.commit_block(self.current_block);
            self.busy = false;
        }
        
        // Bit 3: COMMIT_ALL
        if value & (1 << 3) != 0 {
            self.busy = true;
            self.commit_all();
            self.busy = false;
        }
    }
    
    /// Commit all dirty blocks to disk
    pub fn commit_all(&mut self) {
        let dirty_blocks: Vec<u16> = self.blocks
            .iter()
            .filter(|(_, block)| block.dirty)
            .map(|(&num, _)| num)
            .collect();
        
        for block_num in dirty_blocks {
            let _ = self.commit_block(block_num);
        }
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