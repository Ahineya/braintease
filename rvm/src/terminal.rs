//! Synchronous raw console DMA. The line discipline belongs to the guest OS.
use crate::devices::{guest_range, GuestMemory, STATUS_BAD_DESCRIPTOR, STATUS_IO};
use std::io::{self, Write};
use std::ops::Range;

pub const COMMAND: u16 = 14;
pub const MAX_OCTETS: usize = 4096;

pub fn execute<M: GuestMemory + ?Sized>(
    memory: &mut M,
    bank_size: u16,
    descriptor: Range<usize>,
    words: &[u16],
) -> Result<(), u16> {
    write(
        memory,
        bank_size,
        descriptor,
        words,
        &mut io::stdout().lock(),
    )
}

fn write<M: GuestMemory + ?Sized>(
    memory: &mut M,
    bank_size: u16,
    descriptor: Range<usize>,
    words: &[u16],
    output: &mut impl Write,
) -> Result<(), u16> {
    let count = usize::from(words[4]);
    if count > MAX_OCTETS || words[5..8].iter().any(|&v| v != 0) {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let source = guest_range(memory.len(), bank_size, words[2], words[3], count)?;
    if source.start < descriptor.end && descriptor.start < source.end {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let bytes: Vec<u8> = source.map(|at| memory.read(at) as u8).collect();
    if memory.buffer_console_output(&bytes) {
        memory.write(descriptor.start + 6, count as u16);
        memory.record_console_output(&bytes);
        return Ok(());
    }
    let mut done = 0;
    while done < bytes.len() {
        match output.write(&bytes[done..]) {
            Ok(0) => break,
            Ok(n) => done += n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(_) => break,
        }
    }
    // Even a failed transfer reports irreversible progress: never repeat it.
    memory.write(descriptor.start + 6, done as u16);
    memory.record_console_output(&bytes[..done]);
    if done != count || output.flush().is_err() {
        return Err(STATUS_IO);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn raw_octets_cross_banks_and_preserve_controls() {
        let mut ram = vec![0; 256];
        ram[127..133].copy_from_slice(&[0, 13, 10, 27, 255, 0x1234]);
        let before = ram.clone();
        let mut output = Vec::new();
        write(
            &mut ram[..],
            64,
            40..48,
            &[1, 8, 1, 63, 6, 0, 0, 0],
            &mut output,
        )
        .unwrap();
        assert_eq!(output, [0, 13, 10, 27, 255, 0x34]);
        assert_eq!(ram[46], 6);
        ram[46] = before[46];
        assert_eq!(ram, before);
    }
    #[test]
    fn invalid_transfers_have_no_output_or_memory_effects() {
        for words in [
            [1, 8, 0, 40, 8, 0, 0, 0],
            [1, 8, 4, 0, 1, 0, 0, 0],
            [1, 8, 1, 64, 1, 0, 0, 0],
            [1, 8, 1, 0, 4097, 0, 0, 0],
            [1, 8, 1, 0, 1, 1, 0, 0],
            [1, 8, 1, 0, 1, 0, 1, 0],
        ] {
            let mut ram = vec![9; 256];
            let before = ram.clone();
            let mut out = Vec::new();
            assert!(write(&mut ram[..], 64, 40..48, &words, &mut out).is_err());
            assert!(out.is_empty());
            assert_eq!(ram, before);
        }
    }
    #[test]
    fn short_writes_interruptions_and_failures_report_progress() {
        struct Sink {
            bytes: Vec<u8>,
            calls: usize,
        }
        impl Write for Sink {
            fn write(&mut self, b: &[u8]) -> io::Result<usize> {
                self.calls += 1;
                if self.calls == 1 {
                    return Err(io::ErrorKind::Interrupted.into());
                }
                if self.bytes.len() == 2 {
                    return Err(io::ErrorKind::BrokenPipe.into());
                }
                self.bytes.push(b[0]);
                Ok(1)
            }
            fn flush(&mut self) -> io::Result<()> {
                Ok(())
            }
        }
        let mut ram = vec![65; 256];
        let mut out = Sink {
            bytes: vec![],
            calls: 0,
        };
        assert_eq!(
            write(
                &mut ram[..],
                64,
                40..48,
                &[1, 8, 1, 0, 4, 0, 0, 0],
                &mut out
            ),
            Err(STATUS_IO)
        );
        assert_eq!(out.bytes, b"AA");
        assert_eq!(ram[46], 2);
    }
}
