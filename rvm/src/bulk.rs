//! Bounded, synchronous DMA-style transfers. Filesystems and executable formats
//! remain guest policy; the host sees only disk octets and architectural cells.
use crate::devices::*;
use crate::vm::Storage;
use std::ops::Range;

pub const STORAGE_READ: u16 = 0x0300;
pub const STORAGE_WRITE: u16 = 0x0301;
pub const MEMORY_COPY: u16 = 0x0302;
pub const MEMORY_FILL: u16 = 0x0303;
pub const MEMORY_INFO: u16 = 0x0304;
pub const CAP_STORAGE: u16 = 0x0200;
pub const CAP_MEMORY: u16 = 0x0400;
pub const CAP_CODE: u16 = 0x0800;
pub const WORDS: u16 = 0;
pub const OCTETS: u16 = 1;
pub const CODE_WORDS: u16 = 2;
pub const CODE_OCTETS: u16 = 3;
pub const MAX_CELLS: usize = 32768;
pub const MAX_INSTRUCTIONS: usize = 8192;
pub const DESCRIPTOR_CELLS: usize = 12;

pub fn descriptor_length(command: u16) -> Option<usize> {
    if command == MEMORY_INFO { return Some(8); }
    matches!(
        command,
        STORAGE_READ | STORAGE_WRITE | MEMORY_COPY | MEMORY_FILL
    )
    .then_some(DESCRIPTOR_CELLS)
}

pub fn capabilities<M: GuestMemory + ?Sized>(memory: &M, bank_size: u16, disk: bool) -> u16 {
    CAP_MEMORY
        | if disk { CAP_STORAGE } else { 0 }
        | if memory.code_capacity(bank_size) != 0 {
            CAP_CODE
        } else {
            0
        }
}

fn overlap(a: &Range<usize>, b: &Range<usize>) -> bool {
    a.start < b.end && b.start < a.end
}

fn code_range<M: GuestMemory + ?Sized>(
    memory: &M,
    size: u16,
    bank: u16,
    off: u16,
    count: usize,
) -> Result<Range<usize>, u16> {
    let capacity = memory.code_capacity(size);
    if capacity == 0 {
        return Err(STATUS_UNSUPPORTED);
    }
    if size == 0 || off >= size {
        return Err(STATUS_BOUNDS);
    }
    let start = usize::from(bank) * usize::from(size) + usize::from(off);
    let end = start.checked_add(count).ok_or(STATUS_BOUNDS)?;
    if end > capacity || end > usize::from(size) * 65536 {
        return Err(STATUS_BOUNDS);
    }
    Ok(start..end)
}

fn disk_range(offset: u32, count: usize) -> Result<(), u16> {
    if u64::from(offset) + count as u64 > 1u64 << 32 {
        return Err(STATUS_BOUNDS);
    }
    Ok(())
}

pub fn execute<M: GuestMemory + ?Sized>(
    command: u16,
    memory: &mut M,
    descriptor: Range<usize>,
    bank_size: u16,
    storage: Option<&mut Storage>,
) -> Result<(), u16> {
    if command == MEMORY_INFO {
        let architectural = usize::from(bank_size) * 65536;
        let ram = memory.len().min(architectural) as u32;
        let code = memory.code_capacity(bank_size).min(architectural) as u32;
        let output = [1, 8, ram as u16, (ram >> 16) as u16,
                      code as u16, (code >> 16) as u16, bank_size, 0];
        for (index, word) in descriptor.zip(output) { memory.write(index, word); }
        return Ok(());
    }
    // Snapshot before any mutation. Reject RAM destination aliases with the
    // descriptor rather than having command submission overwrite its own ABI.
    let mut d = [0u16; DESCRIPTOR_CELLS];
    for (word, index) in d.iter_mut().zip(descriptor.clone()) {
        *word = memory.read(index);
    }
    let count = (u32::from(d[6]) | (u32::from(d[7]) << 16)) as usize;
    let format = d[8];
    if d[9..].iter().any(|&v| v != 0) {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let code = match (command, format) {
        (STORAGE_READ, WORDS | OCTETS)
        | (STORAGE_WRITE, WORDS | OCTETS)
        | (MEMORY_COPY, WORDS | OCTETS)
        | (MEMORY_FILL, WORDS) => false,
        (STORAGE_READ, CODE_WORDS) | (MEMORY_COPY, CODE_WORDS | CODE_OCTETS) => true,
        _ => return Err(STATUS_BAD_DESCRIPTOR),
    };
    if count > if code { MAX_INSTRUCTIONS } else { MAX_CELLS } {
        return Err(STATUS_BOUNDS);
    }
    let destination = if code {
        code_range(memory, bank_size, d[4], d[5], count)?
    } else {
        guest_range(memory.len(), bank_size, d[4], d[5], count)?
    };
    if !code && command != STORAGE_WRITE && overlap(&destination, &descriptor) {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let cells = match command {
        STORAGE_READ | STORAGE_WRITE => {
            let offset = u32::from(d[2]) | (u32::from(d[3]) << 16);
            let bytes_per_element = if code {
                8
            } else if format == WORDS {
                2
            } else {
                1
            };
            let bytes_count = count * bytes_per_element;
            disk_range(offset, bytes_count)?;
            let storage = storage.ok_or(STATUS_UNSUPPORTED)?;
            if command == STORAGE_WRITE {
                let mut bytes = Vec::with_capacity(bytes_count);
                for i in destination {
                    let value = memory.read(i);
                    bytes.push(value as u8);
                    if format == WORDS {
                        bytes.push((value >> 8) as u8);
                    }
                }
                return storage
                    .write_bytes_at(offset, &bytes)
                    .map_err(|_| STATUS_IO);
            }
            let bytes = storage
                .read_bytes_at(offset, bytes_count)
                .map_err(|_| STATUS_IO)?;
            if format == OCTETS {
                bytes.into_iter().map(u16::from).collect::<Vec<_>>()
            } else {
                bytes
                    .chunks_exact(2)
                    .map(|p| u16::from_le_bytes([p[0], p[1]]))
                    .collect()
            }
        }
        MEMORY_COPY => {
            let width = if !code {
                1
            } else if format == CODE_OCTETS {
                8
            } else {
                4
            };
            let source = guest_range(memory.len(), bank_size, d[2], d[3], count * width)?;
            if format == CODE_OCTETS {
                source
                    .step_by(2)
                    .map(|i| (memory.read(i) & 255) | ((memory.read(i + 1) & 255) << 8))
                    .collect()
            } else {
                source
                    .map(|i| {
                        if format == OCTETS {
                            memory.read(i) & 255
                        } else {
                            memory.read(i)
                        }
                    })
                    .collect()
            }
        }
        MEMORY_FILL => {
            if d[3] != 0 {
                return Err(STATUS_BAD_DESCRIPTOR);
            }
            vec![d[2]; count]
        }
        _ => return Err(STATUS_UNKNOWN_COMMAND),
    };
    if count == 0 {
        return Ok(());
    }
    if code {
        let instructions: Vec<[u16; 4]> = cells
            .chunks_exact(4)
            .map(|w| [w[0] & 255, w[1], w[2], w[3]])
            .collect();
        memory.write_code(bank_size, destination.start, &instructions)
    } else {
        for (i, value) in destination.zip(cells) {
            memory.write(i, value);
        }
        Ok(())
    }
}
