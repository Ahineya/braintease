//! ABI 2: bounded, stateless AES-GCM records. Authenticate before guest writes.
use super::*;
use crate::devices::STATUS_BAD_STATE;
use aes_gcm::{
    aead::{AeadInPlace, KeyInit},
    Aes128Gcm, Aes256Gcm, Nonce, Tag,
};

pub(super) const HEADER: usize = 130;
pub(super) const MAX: usize = 18432;

pub(super) fn execute<M: GuestMemory + ?Sized>(
    memory: &mut M,
    range: Range<usize>,
) -> Result<(), u16> {
    if range.len() < HEADER {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let h: Vec<u16> = (range.start..range.start + 6)
        .map(|i| memory.read(i))
        .collect();
    let size = h[4] as usize;
    if h[0] != 2
        || !matches!(h[1], 13 | 14)
        || !matches!(h[2], 16 | 32)
        || h[3] > 64
        || size > MAX
        || h[5] != 0
        || range.len() != HEADER + size
    {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let read = |offset: usize, count: usize| -> Result<Zeroizing<Vec<u8>>, u16> {
        let mut data = Zeroizing::new(Vec::with_capacity(count));
        for i in range.start + offset..range.start + offset + count {
            data.push(u8::try_from(memory.read(i)).map_err(|_| STATUS_BAD_DESCRIPTOR)?);
        }
        Ok(data)
    };
    let key = read(6, h[2] as usize)?;
    let nonce = read(38, 12)?;
    let aad = read(66, h[3] as usize)?;
    let mut data = read(HEADER, size)?;
    let incoming_tag = if h[1] == 14 {
        read(50, 16)?
    } else {
        Zeroizing::new(vec![0; 16])
    };
    let mut outgoing_tag = Zeroizing::new([0u8; 16]);
    macro_rules! process {
        ($cipher:ty) => {{
            let cipher = <$cipher>::new_from_slice(&key).map_err(|_| STATUS_BAD_DESCRIPTOR)?;
            if h[1] == 13 {
                let tag = cipher
                    .encrypt_in_place_detached(Nonce::from_slice(&nonce), &aad, &mut data)
                    .map_err(|_| STATUS_BAD_DESCRIPTOR)?;
                outgoing_tag.copy_from_slice(&tag);
            } else {
                cipher
                    .decrypt_in_place_detached(
                        Nonce::from_slice(&nonce),
                        &aad,
                        &mut data,
                        Tag::from_slice(&incoming_tag),
                    )
                    .map_err(|_| STATUS_BAD_STATE)?;
            }
        }};
    }
    if h[2] == 16 {
        process!(Aes128Gcm);
    } else {
        process!(Aes256Gcm);
    }
    // No fallible work after this point; a rejected tag never releases plaintext.
    for (i, &octet) in data.iter().enumerate() {
        memory.write(range.start + HEADER + i, octet.into());
    }
    if h[1] == 13 {
        for (i, &octet) in outgoing_tag.iter().enumerate() {
            memory.write(range.start + 50 + i, octet.into());
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn request(size: usize) -> Vec<u16> {
        let mut ram = vec![0xaaaa; HEADER + size + 200];
        ram[100..100 + HEADER + size].fill(0);
        ram[100..106].copy_from_slice(&[2, 13, 16, 0, size as u16, 0]);
        ram
    }
    #[test]
    fn nist_record_and_rejected_tag_are_atomic() {
        let mut ram = request(16);
        execute(&mut ram[..], 100..100 + HEADER + 16).unwrap();
        let cipher = [
            0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92, 0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2,
            0xfe, 0x78,
        ];
        let tag = [
            0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd, 0xf5, 0x3a, 0x67, 0xb2, 0x12, 0x57,
            0xbd, 0xdf,
        ];
        assert_eq!(&ram[100 + HEADER..100 + HEADER + 16], &cipher);
        assert_eq!(&ram[150..166], &tag);
        assert_eq!(ram[99], 0xaaaa);
        assert_eq!(ram[100 + HEADER + 16], 0xaaaa);
        ram[101] = 14;
        ram[150] ^= 1;
        let before = ram.clone();
        assert_eq!(
            execute(&mut ram[..], 100..100 + HEADER + 16),
            Err(STATUS_BAD_STATE)
        );
        assert_eq!(ram, before);
        ram[150] ^= 1;
        execute(&mut ram[..], 100..100 + HEADER + 16).unwrap();
        assert!(ram[100 + HEADER..100 + HEADER + 16].iter().all(|&x| x == 0));
    }
    #[test]
    fn empty_partial_and_maximum_records_validate_ranges_and_octets() {
        for key in [16, 32] {
            for size in [0, 1, 15, 17, 16384, MAX] {
                let mut ram = request(size);
                ram[102] = key;
                ram[103] = 5;
                for i in 0..size {
                    ram[100 + HEADER + i] = (i & 255) as u16;
                }
                let plain = ram[100 + HEADER..100 + HEADER + size].to_vec();
                execute(&mut ram[..], 100..100 + HEADER + size).unwrap();
                ram[101] = 14;
                execute(&mut ram[..], 100..100 + HEADER + size).unwrap();
                assert_eq!(&ram[100 + HEADER..100 + HEADER + size], &plain);
                for offset in [6, 38, 50, 66] {
                    ram[100 + offset] = 256;
                    let before = ram.clone();
                    assert_eq!(
                        execute(&mut ram[..], 100..100 + HEADER + size),
                        Err(STATUS_BAD_DESCRIPTOR)
                    );
                    assert_eq!(ram, before);
                    ram[100 + offset] = 0;
                }
                let before = ram.clone();
                assert_eq!(
                    execute(&mut ram[..], 100..100 + HEADER + size - 1),
                    Err(STATUS_BAD_DESCRIPTOR)
                );
                assert_eq!(ram, before);
            }
        }
    }
    #[test]
    fn device_bank_crossing_overlap_and_bad_shape_are_atomic() {
        let start = 63980;
        let cells = HEADER + 17;
        let mut ram = vec![0xaaaa; 130000];
        ram[start..start + cells].fill(0);
        ram[start..start + 6].copy_from_slice(&[2, 13, 16, 0, 17, 0]);
        let descriptor = [1, 6, 0, start as u16, cells as u16, 0];
        crate::crypto::execute(&mut ram[..], 64000, 100..106, &descriptor).unwrap();
        assert_eq!(ram[start - 1], 0xaaaa);
        assert_eq!(ram[start + cells], 0xaaaa);
        ram[start + 1] = 14;
        let before = ram.clone();
        assert_eq!(
            crate::crypto::execute(&mut ram[..], 64000, start..start + 6, &descriptor),
            Err(STATUS_BAD_DESCRIPTOR)
        );
        assert_eq!(ram, before);
        for field in [0, 1, 2, 3, 4, 5] {
            let original = ram[start + field];
            ram[start + field] = 65535;
            let before = ram.clone();
            assert_eq!(
                crate::crypto::execute(&mut ram[..], 64000, 100..106, &descriptor),
                Err(STATUS_BAD_DESCRIPTOR)
            );
            assert_eq!(ram, before);
            ram[start + field] = original;
        }
    }
}
