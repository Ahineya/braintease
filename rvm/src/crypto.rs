//! Stateless crypto coprocessor. TLS, certificate policy and entropy stay in the guest.
use crate::devices::{guest_range, GuestMemory, STATUS_BAD_DESCRIPTOR, STATUS_UNSUPPORTED};
use num_bigint::BigUint;
use p256::ecdsa::signature::hazmat::PrehashVerifier;
use p256::elliptic_curve::sec1::ToEncodedPoint;
use std::ops::Range;
use zeroize::Zeroizing;
mod gcm;
mod primitives;

pub const COMMAND: u16 = 13;
pub const CELLS: usize = 8 + 4 * 512;

fn calculate(op: u16, curve: u16, a: &[u8], b: &[u8], c: &[u8]) -> Result<Vec<u8>, u16> {
    if op == 0 {
        if curve > 1 || !a.is_empty() || !b.is_empty() || !c.is_empty() {
            return Err(STATUS_BAD_DESCRIPTOR);
        }
        // Preserve the exact V1 reply for existing TLS device discovery.
        // Extended query: version, low/high operation-mask octets.
        return Ok(if curve == 0 {
            vec![1, 7, 0x1e]
        } else {
            vec![1, 0xfe, 0x7f]
        });
    }
    if (5..=12).contains(&op) {
        return primitives::calculate(op, curve, a, b, c);
    }
    if op == 4 {
        if curve != 0
            || ![256, 384, 512].contains(&b.len())
            || a.is_empty()
            || a.len() > b.len()
            || c.is_empty()
            || c.len() > 8
            || b[0] & 0x80 == 0
            || b[b.len() - 1] & 1 == 0
        {
            return Err(STATUS_BAD_DESCRIPTOR);
        }
        let n = BigUint::from_bytes_be(b);
        let e = BigUint::from_bytes_be(c);
        let x = BigUint::from_bytes_be(a);
        if e < BigUint::from(3u8) || c[c.len() - 1] & 1 == 0 || x >= n {
            return Err(STATUS_BAD_DESCRIPTOR);
        }
        let result = x.modpow(&e, &n).to_bytes_be();
        let mut out = vec![0; b.len()];
        out[b.len() - result.len()..].copy_from_slice(&result);
        return Ok(out);
    }
    // Concrete curve types let RustCrypto validate scalars and SEC1 points.
    macro_rules! ecc {
        ($module:ident, $size:expr) => {{
            match op {
                1 | 2 => {
                    if a.len() != $size
                        || !c.is_empty()
                        || (op == 1 && !b.is_empty())
                        || (op == 2 && (b.len() != 2 * $size + 1 || b[0] != 4))
                    {
                        return Err(STATUS_BAD_DESCRIPTOR);
                    }
                    let key =
                        $module::SecretKey::from_slice(a).map_err(|_| STATUS_BAD_DESCRIPTOR)?;
                    if op == 1 {
                        Ok(key.public_key().to_encoded_point(false).as_bytes().to_vec())
                    } else {
                        let peer = $module::PublicKey::from_sec1_bytes(b)
                            .map_err(|_| STATUS_BAD_DESCRIPTOR)?;
                        let shared = $module::ecdh::diffie_hellman(
                            key.to_nonzero_scalar(),
                            peer.as_affine(),
                        );
                        Ok(shared.raw_secret_bytes().to_vec())
                    }
                }
                3 => {
                    if a.len() != 2 * $size + 1
                        || a[0] != 4
                        || !(20..=64).contains(&b.len())
                        || c.len() != 2 * $size
                    {
                        return Err(STATUS_BAD_DESCRIPTOR);
                    }
                    let key = $module::ecdsa::VerifyingKey::from_sec1_bytes(a)
                        .map_err(|_| STATUS_BAD_DESCRIPTOR)?;
                    let valid = $module::ecdsa::Signature::from_slice(c)
                        .map(|sig| key.verify_prehash(b, &sig).is_ok())
                        .unwrap_or(false);
                    Ok(vec![u8::from(valid)])
                }
                _ => Err(STATUS_UNSUPPORTED),
            }
        }};
    }
    match curve {
        256 => ecc!(p256, 32),
        384 => ecc!(p384, 48),
        521 => ecc!(p521, 66),
        _ => Err(STATUS_UNSUPPORTED),
    }
}

pub(crate) fn execute<M: GuestMemory + ?Sized>(
    memory: &mut M,
    bank_size: u16,
    descriptor_range: Range<usize>,
    descriptor: &[u16],
) -> Result<(), u16> {
    if descriptor[5] != 0 {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let range = guest_range(
        memory.len(),
        bank_size,
        descriptor[2],
        descriptor[3],
        descriptor[4] as usize,
    )?;
    if range.start < descriptor_range.end && descriptor_range.start < range.end {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    if range.len() >= gcm::HEADER && memory.read(range.start) == 2 {
        return gcm::execute(memory, range);
    }
    if range.len() != CELLS {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let header: Vec<u16> = (range.start..range.start + 8)
        .map(|i| memory.read(i))
        .collect();
    if header[0] != 1 || header[7] != 0 || header[3..7].iter().any(|&n| n > 512) {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let mut inputs = Zeroizing::new(Vec::new());
    for field in 0..3 {
        let start = range.start + 8 + field * 512;
        let mut data = Zeroizing::new(Vec::with_capacity(header[3 + field] as usize));
        for i in start..start + header[3 + field] as usize {
            data.push(u8::try_from(memory.read(i)).map_err(|_| STATUS_BAD_DESCRIPTOR)?);
        }
        inputs.push(data);
    }
    let required = match header[1] {
        5 | 11 => 256,
        12 => 8,
        6 | 10 => 64,
        7 | 8 => header[3] as usize,
        9 => 32,
        _ => 0,
    };
    if required > header[6] as usize {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let result = Zeroizing::new(calculate(
        header[1], header[2], &inputs[0], &inputs[1], &inputs[2],
    )?);
    if result.len() > header[6] as usize {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    // Validate and calculate completely before modifying any guest cell.
    for (i, &octet) in result.iter().enumerate() {
        memory.write(range.start + 8 + 3 * 512 + i, octet.into());
    }
    memory.write(range.start + 6, result.len() as u16);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn extended_discovery_preserves_legacy_and_new_failures_are_atomic() {
        assert_eq!(calculate(0, 0, &[], &[], &[]).unwrap(), [1, 7, 30]);
        assert_eq!(calculate(0, 1, &[], &[], &[]).unwrap(), [1, 254, 0x7f]);
        for (op, parameter, na, nb, nc, capacity) in [
            (5, 0, 1, 256, 1, 256),      // Invalid even/zero modulus.
            (6, 0, 0, 0, 4, 64),         // Zero PBKDF2 iterations.
            (6, 0, 0, 0, 4, 63),         // Short output capacity.
            (7, 0, 17, 32, 32, 512),     // Partial AES block.
            (8, 0, 16, 31, 32, 512),     // Short key.
            (9, 0, 0, 1, 0, 32),         // Unexpected hash input.
            (10, 1, 0, 0, 0, 64),        // Unsupported parameter.
            (11, 0, 255, 256, 256, 256), // Short modular operand.
            (11, 0, 256, 256, 256, 255), // Short product output.
            (12, 0, 8, 0, 0, 7),         // Short factor output.
            (12, 1, 1, 0, 0, 8),         // Unsupported parameter.
        ] {
            let mut ram = vec![0u16; 4096];
            ram[100..108].copy_from_slice(&[1, op, parameter, na, nb, nc, capacity, 0]);
            if op == 6 && capacity == 63 {
                // Otherwise-valid derivation with an insufficient output buffer.
                ram[100 + 8 + 1024..100 + 8 + 1028].copy_from_slice(&[0, 0, 0, 1]);
            }
            let before = ram.clone();
            assert_eq!(
                execute(&mut ram[..], 64, 40..46, &[1, 6, 1, 36, CELLS as u16, 0]),
                Err(STATUS_BAD_DESCRIPTOR)
            );
            assert_eq!(ram, before);
        }
    }
    #[test]
    fn all_curves_public_and_ecdh_agree() {
        for (curve, n) in [(256, 32), (384, 48), (521, 66)] {
            let mut one = vec![0; n];
            one[n - 1] = 1;
            let mut two = one.clone();
            two[n - 1] = 2;
            let public = calculate(1, curve, &two, &[], &[]).unwrap();
            let shared = calculate(2, curve, &one, &public, &[]).unwrap();
            assert_eq!(shared, &public[1..n + 1]);
            assert!(calculate(1, curve, &vec![0; n], &[], &[]).is_err());
            assert!(calculate(2, curve, &one, &vec![4; 2 * n + 1], &[]).is_err());
        }
    }
    #[test]
    fn request_is_atomic_and_rejects_wide_octets() {
        let mut ram = vec![0xaaaa; 4096];
        ram[100..108].copy_from_slice(&[1, 1, 256, 32, 0, 0, 65, 0]);
        ram[108..140].fill(0);
        ram[139] = 1;
        execute(&mut ram[..], 64, 40..46, &[1, 6, 1, 36, CELLS as u16, 0]).unwrap();
        assert_eq!(ram[100 + 8 + 1536], 4);
        assert_eq!(ram[100 + 8 + 1536 + 65], 0xaaaa);
        ram[108] = 256;
        let before = ram.clone();
        assert_eq!(
            execute(&mut ram[..], 64, 40..46, &[1, 6, 1, 36, CELLS as u16, 0]),
            Err(STATUS_BAD_DESCRIPTOR)
        );
        assert_eq!(ram, before);
        assert!(execute(&mut ram[..], 64, 100..106, &[1, 6, 1, 36, CELLS as u16, 0]).is_err());
    }
    #[test]
    fn malformed_requests_preserve_all_memory() {
        use crate::devices::STATUS_BOUNDS;
        for (d, error) in [
            ([1, 6, 0, 0, CELLS as u16, 0], STATUS_BOUNDS),
            ([1, 6, 63, 63, CELLS as u16, 0], STATUS_BOUNDS),
            ([1, 6, 1, 64, CELLS as u16, 0], STATUS_BOUNDS),
            ([1, 6, 1, 36, 0, 0], STATUS_BAD_DESCRIPTOR),
            ([1, 6, 1, 36, CELLS as u16, 1], STATUS_BAD_DESCRIPTOR),
        ] {
            let mut ram = vec![0xaaaa; 4096];
            let before = ram.clone();
            assert_eq!(execute(&mut ram[..], 64, 40..46, &d), Err(error));
            assert_eq!(ram, before);
        }
        let mut ram = vec![0xaaaa; 4096];
        // Valid query with insufficient output capacity must be atomic too.
        ram[100..108].copy_from_slice(&[1, 0, 0, 0, 0, 0, 2, 0]);
        let before = ram.clone();
        assert_eq!(
            execute(&mut ram[..], 64, 40..46, &[1, 6, 1, 36, CELLS as u16, 0]),
            Err(STATUS_BAD_DESCRIPTOR)
        );
        assert_eq!(ram, before);
    }
    #[test]
    fn rsa_public_matches_python_known_answers() {
        // Python pow(0x123456789abcdef, 65537, 2**bits-159), fixed-width BE.
        for (size, expected) in [
            (256, "e66012be3ac087d4a41362ccbbb11f83265556aff8032ffed5eb683ec58f2f61f188d515b3a8e7bc90e954de2d39c85be5f9b380c9bb3ae1f83617015ed41da7695b2db767dd0bd853a8b683118ff1a42f97019cbd9dcc8e4242c62e5cacf8458d55a20697b71c1af3d0de0f96e3d59c3ed15a3653aa74ff8203f78064888f0f564c25e5086b498f89fe8a08e423ca0bfc4a0934f8c2d2e535541a6cdf4f8f29881825a62f479a8a7e44449a641f47c57dc4c5e410616e670834b6d98009d00dfbd1801c979c159d80d00a28043a41d93684ee7bc7f4e149f5db97394d7e7fffd000de29a14eb62434c5be6f87559a3bb8e350758eec32a23d6748a8f1f8bab9"),
            (384, "2d4dc8a95ab4c37ac52e353ed8bfe27d25a3fa0ed00ee0a4b8e8bdc97c640c9b2112def3e868552c8fcd80f5df5d6a066bb84fd95e59fc1d1a0fda0ab4d503574c58aa8b990a9f26ca24427f76db1668fa5b691027488067b8abfd6682e3b6a94c1156dccf4f1b54ce092a4920e1fcbd8865e06f63a3cffca5ca2d4c85d4cf09af6103c9484fb175d8968ba2adaf98f8f24bb1acc2475619c3598e75325f9a7a63edcb38ba48a5f86f3a9b0eeaa035cd41768984ebd430b738467481bb0ef236d9323b274ff75c5b247b58f96b91783d3dd8f9339384161105de2beea9c9c9a10b080ed70e4cbb132d71c5c42fa2abf8c0ec87c19e1e9ec35ea98d1655375844503d2fc8ed9a71ec2b19860b2b6c6994612f0f0c1b36e0b5fde0bbfded9212f3070db1f1154b14db48e0fbcc6c1ab098f6f2e33b088faa0c787c22e414d12a62b32e16f69a1976d8341ba7e4f83542903f4d5ebbb234a3a5d35d6b2371290fd114c00b58082e8ee832b34c209b646d1401cd93478df737d6a7075694ffcd4055"),
            (512, "13fa4a1d7df1378ed33cc68e53da2e96fa636fde1f657ed5696c6d3ded61575fed86e7f70404e8ec44d96b8af8e777217f30a0d8d92e2aed054befb066e4bb9528b30201e4a0b4c00be5ad9fb0977eac146db80567938b68a88fce962b9292815a03b1321391a04ee6a9e5b0372552307ea5d285669a5fa5f4d5236202cec052709c7109dd273e95a2b405f4f5744cd4aa980876a551f876705beb73356e5cde99ad508da23dec73e2722d245bc7e622707bdb370e989cf3809052b95409f4c9fe54d19cf7b6cc499f9aea57b45a02225dfe1ff0d859b913dab18b68ed377b3cf047058299df605580ec92876d055671889fda47e28c938930e7dd5c36e09935cab6771f00d173d4a31f34478efab6e322f9bf95a3a873a15fc952ade25ba5e9ca6f4a549e657f21103d8ff417d4c4d839a9844e6c569d0ba8dbfe98469c7152d112f9a70a251ef3017035a1e14acd425b2b07f3a3094ef9c230ed79c29a55d8f671c44ef2c5d26c025f40402b3e64f4e18a424e7b6819c25bf924442230bf16c8e9739514e537088e1d0849d7c86918c7206644fe85506d24dfd68b55f32b08b4a0d20e9926c7b49b2afece980dc690167cd56f6d99e84522421d10109ded43d45d76c5b8e2c5ecc8a9c20700ecaaf8c2ff7a205d99a845890814e6c803693a73a612d8187942dfc795b5be31eb2d31c5af190546f433b0f17cf5e1bb444566"),
        ] {
            let mut modulus=vec![255;size];modulus[size-1]=97;
            let actual=calculate(4,0,&[1,35,69,103,137,171,205,239],&modulus,&[1,0,1]).unwrap();
            let hex:String=actual.iter().map(|b|format!("{b:02x}")).collect();
            assert_eq!(hex,expected);
        }
    }
    #[test]
    fn invalid_signature_is_a_result_not_a_device_failure() {
        let mut scalar = [0; 32];
        scalar[31] = 1;
        let public = calculate(1, 256, &scalar, &[], &[]).unwrap();
        assert_eq!(calculate(3, 256, &public, &[1; 32], &[0; 64]).unwrap(), [0]);
    }
}
