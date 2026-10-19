//! Host OS entropy, separate from the seeded, reproducible VM RNG.
use crate::devices::{guest_range, GuestMemory, STATUS_BAD_DESCRIPTOR, STATUS_IO};
use std::ops::Range;

pub const MAX_OCTETS: usize = 256;

pub(crate) fn execute<M: GuestMemory + ?Sized>(
    memory: &mut M,
    bank_size: u16,
    descriptor_range: Range<usize>,
    descriptor: &[u16],
    fill: impl FnOnce(&mut [u8]) -> Result<(), ()>,
) -> Result<(), u16> {
    let count = usize::from(descriptor[4]);
    if count == 0 || count > MAX_OCTETS || descriptor[5] != 0 {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let output = guest_range(memory.len(), bank_size, descriptor[2], descriptor[3], count)?;
    if output.start < descriptor_range.end && descriptor_range.start < output.end {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    // Stage the complete request. A failed OS call must not expose a partial
    // result, even if its provider modified the temporary buffer first.
    let mut octets = [0u8; MAX_OCTETS];
    fill(&mut octets[..count]).map_err(|_| STATUS_IO)?;
    for (address, octet) in output.zip(octets) {
        memory.write(address, u16::from(octet));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::devices::STATUS_BOUNDS;

    #[test]
    fn octets_cross_banks_without_touching_guards() {
        let mut ram = vec![0xaaaa; 512];
        execute(&mut ram[..], 64, 40..46, &[1, 6, 1, 63, 4, 0], |p| {
            p.copy_from_slice(&[0, 127, 128, 255]);
            Ok(())
        })
        .unwrap();
        assert_eq!(&ram[126..132], &[0xaaaa, 0, 127, 128, 255, 0xaaaa]);
    }
    #[test]
    fn rejects_all_bad_ranges_before_requesting_entropy() {
        for (d, error) in [
            ([1, 6, 1, 0, 0, 0], STATUS_BAD_DESCRIPTOR),
            ([1, 6, 1, 0, 257, 0], STATUS_BAD_DESCRIPTOR),
            ([1, 6, 1, 0, 16, 1], STATUS_BAD_DESCRIPTOR),
            ([1, 6, 0, 40, 16, 0], STATUS_BAD_DESCRIPTOR),
            ([1, 6, 0, 0, 16, 0], STATUS_BOUNDS),
            ([1, 6, 7, 63, 16, 0], STATUS_BOUNDS),
            ([1, 6, 1, 64, 16, 0], STATUS_BOUNDS),
        ] {
            let mut ram = vec![0xaaaa; 512];
            let before = ram.clone();
            assert_eq!(
                execute(&mut ram[..], 64, 40..46, &d, |_| panic!(
                    "invalid entropy request"
                )),
                Err(error)
            );
            assert_eq!(ram, before);
        }
    }
    #[test]
    fn provider_failure_preserves_every_guest_cell() {
        let mut ram = vec![0xaaaa; 512];
        let before = ram.clone();
        assert_eq!(
            execute(&mut ram[..], 64, 40..46, &[1, 6, 1, 0, 32, 0], |p| {
                p.fill(0);
                Err(())
            }),
            Err(STATUS_IO)
        );
        assert_eq!(ram, before);
    }
}
