//! Portable logical machine snapshots. Acceleration caches are reconstructed
//! by normal execution; neither host devices nor BF-specific layouts live here.

use super::{Error, Machine, Op, Program};
use std::fmt;
use std::io::{self, Read, Write};

const MAGIC: &[u8; 8] = b"BFCHECK\0";
// Bump when state/continuation semantics or the encoding change incompatibly.
// Execution-only performance improvements do not invalidate a checkpoint.
const VERSION: u32 = 1;
const PAGE_CELLS: usize = 4096;
const ZERO: u8 = 0;
const PERIODIC: u8 = 1;
const BITMAP: u8 = 2;
const RAW: u8 = 3;
const MAX_PERIOD: usize = 64;

#[derive(Debug)]
pub enum CheckpointError {
    Io(io::Error),
    Invalid(&'static str),
    Machine(Error),
}

impl fmt::Display for CheckpointError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => write!(f, "Brainfuck checkpoint I/O: {error}"),
            Self::Invalid(message) => write!(f, "invalid Brainfuck checkpoint: {message}"),
            Self::Machine(error) => error.fmt(f),
        }
    }
}

impl std::error::Error for CheckpointError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io(error) => Some(error),
            Self::Machine(error) => Some(error),
            Self::Invalid(_) => None,
        }
    }
}

impl From<io::Error> for CheckpointError {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

impl From<Error> for CheckpointError {
    fn from(error: Error) -> Self {
        Self::Machine(error)
    }
}

impl Machine {
    /// Save the logical tape, literal IR continuation, dispatch count, pending
    /// input and pointer policy. Deferred writes are materialized without
    /// executing BF; acceleration caches are not part of the format.
    ///
    /// The caller must supply the same program used to run this machine and
    /// save its own I/O and device state separately. Program compatibility is
    /// checked on restore, but this format is not an integrity/authenticity
    /// envelope: a host should checksum its complete checkpoint container.
    pub fn write_checkpoint(
        &mut self,
        program: &Program,
        mut writer: impl Write,
    ) -> Result<(), CheckpointError> {
        validate_continuation(program, self.pc, self.pending_input)?;
        writer.write_all(MAGIC)?;
        writer.write_all(&VERSION.to_le_bytes())?;
        writer.write_all(&program_fingerprint(program))?;
        write_u64(&mut writer, self.tape.len() as u64)?;
        write_u64(&mut writer, self.pointer as u64)?;
        write_u64(&mut writer, self.pc as u64)?;
        write_u64(&mut writer, self.dispatches)?;
        writer.write_all(&[u8::from(self.pending_input) | (u8::from(self.wrapping_tape) << 1)])?;

        let tape = self.tape.materialize();
        let mut zeros = 0u64;
        for page in tape.chunks(PAGE_CELLS) {
            let nonzero = page.iter().filter(|&&value| value != 0).count();
            if nonzero == 0 {
                zeros += 1;
                continue;
            }
            write_zeros(&mut writer, &mut zeros)?;
            write_page(&mut writer, page, nonzero)?;
        }
        write_zeros(&mut writer, &mut zeros)?;
        Ok(())
    }

    /// Restore a checkpoint for exactly the supplied parsed program. Reject
    /// excessive tape sizes before allocating. Read only this machine record,
    /// allowing a host container to place device records immediately after it.
    ///
    /// The saved dispatch count is exact. Subsequent dispatch counts or budget
    /// boundaries may differ while derived scan caches warm up; observable BF
    /// events, logical tape and literal continuation are preserved.
    pub fn read_checkpoint(
        program: &Program,
        mut reader: impl Read,
        max_tape_cells: usize,
    ) -> Result<Self, CheckpointError> {
        if read_array::<8>(&mut reader)? != *MAGIC {
            return Err(CheckpointError::Invalid("wrong magic"));
        }
        if u32::from_le_bytes(read_array(&mut reader)?) != VERSION {
            return Err(CheckpointError::Invalid("unsupported version"));
        }
        if read_array::<16>(&mut reader)? != program_fingerprint(program) {
            return Err(CheckpointError::Invalid("program or optimizer IR differs"));
        }
        let cells = read_usize(&mut reader)?;
        if cells == 0 || cells > max_tape_cells || cells > isize::MAX as usize {
            return Err(CheckpointError::Invalid(
                "tape size exceeds configured bounds",
            ));
        }
        let pointer = read_usize(&mut reader)?;
        if pointer >= cells {
            return Err(CheckpointError::Invalid("pointer is outside the tape"));
        }
        let pc = read_usize(&mut reader)?;
        let dispatches = read_u64(&mut reader)?;
        let flags = read_array::<1>(&mut reader)?[0];
        if flags & !3 != 0 {
            return Err(CheckpointError::Invalid("unknown machine flags"));
        }
        let pending_input = flags & 1 != 0;
        validate_continuation(program, pc, pending_input)?;

        let mut machine = Self::allocate(cells, flags & 2 != 0)?;
        machine.pointer = pointer;
        machine.pc = pc;
        machine.dispatches = dispatches;
        machine.pending_input = pending_input;
        let tape = machine.tape.materialize();
        let page_count = (cells - 1) / PAGE_CELLS + 1;
        let mut page_index = 0;
        while page_index < page_count {
            let tag = read_array::<1>(&mut reader)?[0];
            if tag == ZERO {
                let count = read_usize(&mut reader)?;
                if count == 0 || count > page_count - page_index {
                    return Err(CheckpointError::Invalid("invalid zero-page run"));
                }
                page_index += count;
                continue;
            }
            let start = page_index * PAGE_CELLS;
            let end = start + PAGE_CELLS.min(cells - start);
            read_page(&mut reader, tag, &mut tape[start..end])?;
            page_index += 1;
        }
        Ok(machine)
    }
}

fn validate_continuation(
    program: &Program,
    pc: usize,
    pending_input: bool,
) -> Result<(), CheckpointError> {
    if pc > program.code.len() {
        return Err(CheckpointError::Invalid(
            "instruction pointer is outside the program",
        ));
    }
    if pending_input && (pc == 0 || !matches!(program.code[pc - 1].op, Op::Input)) {
        return Err(CheckpointError::Invalid(
            "pending input has no preceding input instruction",
        ));
    }
    Ok(())
}

// Compatibility fingerprint, not a cryptographic checksum. Stream the complete
// deterministic IR representation, including every fast-path field and parse
// option; no huge temporary debug string or implementation-source hash. Hosts
// protect their enclosing file and original BF source with a cryptographic hash.
// Debug representation changes conservatively reject previous checkpoints.
fn program_fingerprint(program: &Program) -> [u8; 16] {
    use fmt::Write as _;
    struct Fingerprint(u64, u64);
    impl fmt::Write for Fingerprint {
        fn write_str(&mut self, value: &str) -> fmt::Result {
            for byte in value.bytes() {
                self.0 = (self.0 ^ u64::from(byte)).wrapping_mul(0x100000001b3);
                self.1 = (self.1 ^ u64::from(byte))
                    .rotate_left(5)
                    .wrapping_mul(0x517cc1b727220a95);
            }
            Ok(())
        }
    }
    let mut fingerprint = Fingerprint(0xcbf29ce484222325, 0x9e3779b97f4a7c15);
    // Read options explicitly: derive(Debug) is deliberately complete but does
    // not count as a use for dead-code analysis of these compatibility fields.
    write!(
        &mut fingerprint,
        "{}:{}:{program:?}",
        program.optimize_enabled, program.host_hooks_enabled
    )
    .expect("fingerprint formatting is infallible");
    let mut bytes = [0; 16];
    bytes[..8].copy_from_slice(&fingerprint.0.to_le_bytes());
    bytes[8..].copy_from_slice(&fingerprint.1.to_le_bytes());
    bytes
}

fn write_zeros(writer: &mut impl Write, count: &mut u64) -> io::Result<()> {
    if *count != 0 {
        writer.write_all(&[ZERO])?;
        write_u64(writer, *count)?;
        *count = 0;
    }
    Ok(())
}

fn write_page(writer: &mut impl Write, page: &[u16], nonzero: usize) -> io::Result<()> {
    let bitmap_bytes = page.len().div_ceil(8);
    let sparse_bytes = bitmap_bytes + 2 * nonzero;
    let period_limit = MAX_PERIOD.min(page.len() / 2);
    for period in 1..=period_limit {
        if 1 + 2 * period < sparse_bytes.min(2 * page.len())
            && page[period..]
                .iter()
                .enumerate()
                .all(|(i, &value)| value == page[i % period])
        {
            writer.write_all(&[PERIODIC, period as u8])?;
            return write_words(writer, &page[..period]);
        }
    }
    if sparse_bytes >= 2 * page.len() {
        writer.write_all(&[RAW])?;
        return write_words(writer, page);
    }
    writer.write_all(&[BITMAP])?;
    let mut bitmap = [0u8; PAGE_CELLS / 8];
    for (index, &value) in page.iter().enumerate() {
        if value != 0 {
            bitmap[index / 8] |= 1 << (index % 8);
        }
    }
    writer.write_all(&bitmap[..bitmap_bytes])?;
    let mut words = [0; PAGE_CELLS];
    let mut count = 0;
    for &value in page {
        if value != 0 {
            words[count] = value;
            count += 1;
        }
    }
    write_words(writer, &words[..count])
}

fn read_page(reader: &mut impl Read, tag: u8, page: &mut [u16]) -> Result<(), CheckpointError> {
    match tag {
        PERIODIC => {
            let period = usize::from(read_array::<1>(reader)?[0]);
            if period == 0 || period > MAX_PERIOD || period > page.len() / 2 {
                return Err(CheckpointError::Invalid("invalid page period"));
            }
            read_words(reader, &mut page[..period])?;
            for index in period..page.len() {
                page[index] = page[index % period];
            }
        }
        BITMAP => {
            let count = page.len().div_ceil(8);
            let mut bitmap = [0u8; PAGE_CELLS / 8];
            reader.read_exact(&mut bitmap[..count])?;
            let remainder = page.len() % 8;
            if remainder != 0 && bitmap[count - 1] >> remainder != 0 {
                return Err(CheckpointError::Invalid("bitmap extends beyond page"));
            }
            let values = bitmap[..count]
                .iter()
                .map(|byte| byte.count_ones() as usize)
                .sum();
            let mut words = [0; PAGE_CELLS];
            read_words(reader, &mut words[..values])?;
            let mut next = 0;
            for (index, value) in page.iter_mut().enumerate() {
                if bitmap[index / 8] & (1 << (index % 8)) != 0 {
                    if words[next] == 0 {
                        return Err(CheckpointError::Invalid("zero value in nonzero bitmap"));
                    }
                    *value = words[next];
                    next += 1;
                }
            }
        }
        RAW => read_words(reader, page)?,
        _ => return Err(CheckpointError::Invalid("unknown page encoding")),
    }
    Ok(())
}

fn write_words(writer: &mut impl Write, words: &[u16]) -> io::Result<()> {
    let mut bytes = [0u8; PAGE_CELLS * 2];
    for (word, target) in words.iter().zip(bytes.chunks_exact_mut(2)) {
        target.copy_from_slice(&word.to_le_bytes());
    }
    writer.write_all(&bytes[..words.len() * 2])
}

fn read_words(reader: &mut impl Read, words: &mut [u16]) -> io::Result<()> {
    let mut bytes = [0u8; PAGE_CELLS * 2];
    reader.read_exact(&mut bytes[..words.len() * 2])?;
    for (word, source) in words.iter_mut().zip(bytes.chunks_exact(2)) {
        *word = u16::from_le_bytes([source[0], source[1]]);
    }
    Ok(())
}

fn read_array<const N: usize>(reader: &mut impl Read) -> io::Result<[u8; N]> {
    let mut bytes = [0; N];
    reader.read_exact(&mut bytes)?;
    Ok(bytes)
}

fn write_u64(writer: &mut impl Write, value: u64) -> io::Result<()> {
    writer.write_all(&value.to_le_bytes())
}

fn read_u64(reader: &mut impl Read) -> io::Result<u64> {
    Ok(u64::from_le_bytes(read_array(reader)?))
}

fn read_usize(reader: &mut impl Read) -> Result<usize, CheckpointError> {
    read_u64(reader)?
        .try_into()
        .map_err(|_| CheckpointError::Invalid("integer exceeds platform bounds"))
}

#[cfg(test)]
mod tests;
