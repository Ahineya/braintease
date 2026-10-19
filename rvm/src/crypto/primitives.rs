//! Bounded stateless operations. Protocol validation and key ownership stay in the guest.
use super::STATUS_BAD_DESCRIPTOR;
use aes::cipher::{BlockDecrypt, BlockEncrypt, KeyInit};
use crypto_bigint::{
    modular::runtime_mod::{DynResidue, DynResidueParams},
    Encoding, U2048, U4096,
};
use hmac::{Hmac, Mac};
use sha2::{Digest, Sha256, Sha512};
use zeroize::Zeroizing;

pub(super) fn calculate(
    op: u16,
    parameter: u16,
    a: &[u8],
    b: &[u8],
    c: &[u8],
) -> Result<Vec<u8>, u16> {
    if parameter != 0 {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    match op {
        5 => modexp(a, b, c),
        11 => modmul(a, b, c),
        12 if b.is_empty() && c.is_empty() => factor(a),
        6 => pbkdf2(a, b, c),
        7 | 8 => ige(a, b, c, op == 8),
        9 | 10 if b.is_empty() && c.is_empty() => Ok(if op == 9 {
            Sha256::digest(a).to_vec()
        } else {
            Sha512::digest(a).to_vec()
        }),
        _ => Err(STATUS_BAD_DESCRIPTOR),
    }
}

fn modmul(a: &[u8], modulus: &[u8], b: &[u8]) -> Result<Vec<u8>, u16> {
    if a.len() != 256
        || b.len() != 256
        || modulus.len() != 256
        || modulus[0] & 128 == 0
        || modulus[255] & 1 == 0
    {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let n = U2048::from_be_slice(modulus);
    let x = Zeroizing::new(U2048::from_be_slice(a));
    let y = Zeroizing::new(U2048::from_be_slice(b));
    if *x >= n || *y >= n {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let params = DynResidueParams::new(&n);
    let result =
        Zeroizing::new((DynResidue::new(&x, params) * DynResidue::new(&y, params)).retrieve());
    Ok(result.to_be_bytes().to_vec())
}

// Public proof-of-work only. Fixed work budget; never used with secrets.
fn factor(data: &[u8]) -> Result<Vec<u8>, u16> {
    if data.is_empty() || data.len() > 8 {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let n = data.iter().fold(0u64, |n, b| (n << 8) | u64::from(*b));
    if n < 15 || n >= 1u64 << 63 || n & 1 == 0 || prime(n) {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let mut budget = 1_000_000;
    for seed in 1..=16 {
        let (mut x, mut y, mut d) = (2u64, 2u64, 1u64);
        let step = |x: u64| ((u128::from(x) * u128::from(x) + seed) % u128::from(n)) as u64;
        while d == 1 && budget > 0 {
            x = step(x);
            y = step(step(y));
            budget -= 1;
            let (mut a, mut b) = (x.abs_diff(y), n);
            while b != 0 {
                let r = a % b;
                a = b;
                b = r;
            }
            d = a;
        }
        if d > 1 && d < n {
            let p = d.min(n / d);
            let q = d.max(n / d);
            if p == q || q > u64::from(u32::MAX) || !prime(p) || !prime(q) {
                return Err(STATUS_BAD_DESCRIPTOR);
            }
            let mut out = (p as u32).to_be_bytes().to_vec();
            out.extend_from_slice(&(q as u32).to_be_bytes());
            return Ok(out);
        }
        if budget == 0 {
            break;
        }
    }
    Err(STATUS_BAD_DESCRIPTOR)
}
fn prime(n: u64) -> bool {
    if n < 2 {
        return false;
    }
    for p in [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37] {
        if n % p == 0 {
            return n == p;
        }
    }
    let s = (n - 1).trailing_zeros();
    let d = (n - 1) >> s;
    let mul = |a: u64, b: u64| (u128::from(a) * u128::from(b) % u128::from(n)) as u64;
    // Deterministic Miller-Rabin bases for all unsigned 64-bit inputs.
    for a in [2u64, 325, 9375, 28178, 450775, 9780504, 1795265022] {
        if a % n == 0 {
            continue;
        }
        let (mut x, mut b, mut e) = (1, a % n, d);
        while e != 0 {
            if e & 1 != 0 {
                x = mul(x, b);
            }
            b = mul(b, b);
            e >>= 1;
        }
        if x == 1 || x == n - 1 {
            continue;
        }
        let mut passes = false;
        for _ in 1..s {
            x = mul(x, x);
            if x == n - 1 {
                passes = true;
                break;
            }
        }
        if !passes {
            return false;
        }
    }
    true
}

fn modexp(base: &[u8], modulus: &[u8], exponent: &[u8]) -> Result<Vec<u8>, u16> {
    if base.is_empty()
        || base.len() > 256
        || modulus.len() != 256
        || modulus[0] & 0x80 == 0
        || modulus[255] & 1 == 0
        || exponent.is_empty()
        || exponent.len() > 512
    {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let mut padded_base = Zeroizing::new([0u8; 256]);
    padded_base[256 - base.len()..].copy_from_slice(base);
    let mut padded_exponent = Zeroizing::new([0u8; 512]);
    padded_exponent[512 - exponent.len()..].copy_from_slice(exponent);
    let n = U2048::from_be_slice(modulus);
    let x = Zeroizing::new(U2048::from_be_slice(&*padded_base));
    let e = Zeroizing::new(U4096::from_be_slice(&*padded_exponent));
    if *x >= n {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let params = DynResidueParams::new(&n);
    // Always process all 4096 exponent bits. Do not use the variable-time
    // BigUint public-RSA implementation for secret DH/SRP exponents.
    let result = Zeroizing::new(DynResidue::new(&x, params).pow(&e).retrieve());
    Ok(result.to_be_bytes().to_vec())
}

fn pbkdf2(password: &[u8], salt: &[u8], count: &[u8]) -> Result<Vec<u8>, u16> {
    let count: [u8; 4] = count.try_into().map_err(|_| STATUS_BAD_DESCRIPTOR)?;
    let iterations = u32::from_be_bytes(count);
    if !(1..=100_000).contains(&iterations) {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let prf = <Hmac<Sha512> as Mac>::new_from_slice(password).map_err(|_| STATUS_BAD_DESCRIPTOR)?;
    let mut first = prf.clone();
    first.update(salt);
    first.update(&1u32.to_be_bytes());
    let mut u = Zeroizing::new([0u8; 64]);
    u.copy_from_slice(&first.finalize().into_bytes());
    let mut output = Zeroizing::new(*u);
    for _ in 1..iterations {
        let mut next = prf.clone();
        next.update(&*u);
        u.copy_from_slice(&next.finalize().into_bytes());
        for (out, byte) in output.iter_mut().zip(u.iter()) {
            *out ^= byte;
        }
    }
    Ok(output.to_vec())
}

fn ige(data: &[u8], key: &[u8], iv: &[u8], decrypt: bool) -> Result<Vec<u8>, u16> {
    if data.len() % 16 != 0 || key.len() != 32 || iv.len() != 32 {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let aes = aes::Aes256::new_from_slice(key).map_err(|_| STATUS_BAD_DESCRIPTOR)?;
    let mut previous_cipher = Zeroizing::new([0u8; 16]);
    let mut previous_plain = Zeroizing::new([0u8; 16]);
    previous_cipher.copy_from_slice(&iv[..16]);
    previous_plain.copy_from_slice(&iv[16..]);
    let mut output = Vec::with_capacity(data.len());
    for input in data.chunks_exact(16) {
        let mut block = aes::cipher::Block::<aes::Aes256>::default();
        for i in 0..16 {
            block[i] = input[i]
                ^ if decrypt {
                    previous_plain[i]
                } else {
                    previous_cipher[i]
                };
        }
        if decrypt {
            aes.decrypt_block(&mut block);
        } else {
            aes.encrypt_block(&mut block);
        }
        for i in 0..16 {
            block[i] ^= if decrypt {
                previous_cipher[i]
            } else {
                previous_plain[i]
            };
        }
        output.extend_from_slice(&block);
        previous_cipher.copy_from_slice(if decrypt { input } else { &block });
        previous_plain.copy_from_slice(if decrypt { &block } else { input });
        use zeroize::Zeroize;
        block.zeroize();
    }
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn modular_products_and_public_factorization() {
        let n = [255u8; 256];
        let a = [0x12u8; 256];
        let b = [0xf0u8; 256];
        let expected = (num_bigint::BigUint::from_bytes_be(&a)
            * num_bigint::BigUint::from_bytes_be(&b)
            % num_bigint::BigUint::from_bytes_be(&n))
        .to_bytes_be();
        let got = modmul(&a, &n, &b).unwrap();
        assert_eq!(&got[256 - expected.len()..], expected);
        assert!(modmul(&n, &n, &b).is_err());
        for (p, q) in [
            (3u32, 5u32),
            (1000000007, 1000000009),
            (2147483647, 4294967291),
        ] {
            let got = factor(&(u64::from(p) * u64::from(q)).to_be_bytes()).unwrap();
            assert_eq!(&got[..4], p.to_be_bytes());
            assert_eq!(&got[4..], q.to_be_bytes());
        }
        for n in [0u64, 1, 9, 25, 17, 30, u64::MAX] {
            assert!(factor(&n.to_be_bytes()).is_err());
        }
    }
    fn hex(s: &str) -> Vec<u8> {
        s.as_bytes()
            .chunks_exact(2)
            .map(|c| u8::from_str_radix(std::str::from_utf8(c).unwrap(), 16).unwrap())
            .collect()
    }
    #[test]
    fn password_vectors_and_bounds() {
        // Independently reproducible with Python hashlib.pbkdf2_hmac.
        assert_eq!(pbkdf2(b"password",b"salt",&1u32.to_be_bytes()).unwrap(),hex("867f70cf1ade02cff3752599a3a53dc4af34c7a669815ae5d513554e1c8cf252c02d470a285a0501bad999bfe943c08f050235d7d68b1da55e63f73b60a57fce"));
        assert_eq!(pbkdf2(b"password",b"salt",&2u32.to_be_bytes()).unwrap(),hex("e1d9c16aa681708a45f5c7c4e215ceb66e011a2e9f0040713f18aefdb866d53cf76cab2868a39b9f7840edce4fef5a82be67335c77a6068e04112754f27ccf4e"));
        for n in [0u32, 100_001] {
            assert!(pbkdf2(b"", b"", &n.to_be_bytes()).is_err());
        }
    }
    #[test]
    fn secret_exponents_match_independent_public_arithmetic() {
        let mut modulus = [0xff; 256];
        modulus[255] = 0x61;
        let base = [2u8];
        for exponent in [vec![0], vec![1], vec![0x5a; 256], vec![0x81; 257]] {
            let expected = num_bigint::BigUint::from(2u8)
                .modpow(
                    &num_bigint::BigUint::from_bytes_be(&exponent),
                    &num_bigint::BigUint::from_bytes_be(&modulus),
                )
                .to_bytes_be();
            let result = modexp(&base, &modulus, &exponent).unwrap();
            assert_eq!(&result[256 - expected.len()..], expected);
            assert!(result[..256 - expected.len()].iter().all(|x| *x == 0));
        }
        assert!(modexp(&modulus, &modulus, &[1]).is_err());
        modulus[255] = 0x60;
        assert!(modexp(&base, &modulus, &[1]).is_err());
    }
    #[test]
    fn ige_known_aes_answer_and_chunk_continuation() {
        // NIST AES-256 ECB answer is also the first IGE block with zero IV.
        let key = hex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
        let plain = hex("6bc1bee22e409f96e93d7e117393172a");
        let cipher = hex("f3eed1bdb5d2a03c064b5a7e3db181f8");
        assert_eq!(ige(&plain, &key, &[0; 32], false).unwrap(), cipher);
        assert_eq!(ige(&cipher, &key, &[0; 32], true).unwrap(), plain);
        let data: Vec<u8> = (0..512).map(|i| i as u8).collect();
        let all = ige(&data, &key, &[7; 32], false).unwrap();
        let mut iv = all[240..256].to_vec();
        iv.extend_from_slice(&data[240..256]);
        assert_eq!(ige(&data[256..], &key, &iv, false).unwrap(), all[256..]);
        assert_eq!(ige(&all, &key, &[7; 32], true).unwrap(), data);
        assert!(ige(&[0; 17], &key, &[0; 32], false).is_err());
    }
}
