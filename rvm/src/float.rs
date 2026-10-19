//! Stateless IEEE scalar service shared by RVM and native applications.
//! Arithmetic rounds to nearest, ties to even. NaNs are canonicalized; integer
//! conversions saturate with NaN -> zero, matching the software runtime.
//! Transcendentals use host libm; last-bit equality across hosts is not promised.
use crate::devices::{STATUS_BAD_DESCRIPTOR, STATUS_UNSUPPORTED};
pub const COMMAND: u16 = 15;
pub const CELLS: usize = 16;
fn f32_bits(x: f32) -> u64 {
    u64::from(if x.is_nan() { 0x7fc00000 } else { x.to_bits() })
}
fn f64_bits(x: f64) -> u64 {
    if x.is_nan() {
        0x7ff8000000000000
    } else {
        x.to_bits()
    }
}
pub fn evaluate(op: u16, a: u64, b: u64) -> Result<u64, u16> {
    if op == 0 {
        return if a == 0 && b == 0 {
            Ok(7)
        } else {
            Err(STATUS_BAD_DESCRIPTOR)
        };
    }
    let base = op & 0xff00;
    let code = op & 255;
    if base == 0x100 || base == 0x200 {
        if !(1..=19).contains(&code) {
            return Err(STATUS_UNSUPPORTED);
        }
        let binary = (1..=4).contains(&code) || code >= 14;
        if !binary && b != 0 {
            return Err(STATUS_BAD_DESCRIPTOR);
        }
        if base == 0x100 {
            if (!matches!(code, 5..=8 | 13) && a >> 32 != 0) || b >> 32 != 0 {
                return Err(STATUS_BAD_DESCRIPTOR);
            }
            let x = f32::from_bits(a as u32);
            let y = f32::from_bits(b as u32);
            return Ok(match code {
                1 => f32_bits(x + y),
                2 => f32_bits(x - y),
                3 => f32_bits(x * y),
                4 => f32_bits(x / y),
                5 => f32_bits(a as u32 as f32),
                6 => f32_bits(a as i32 as f32),
                7 => f32_bits(a as f32),
                8 => f32_bits(a as i64 as f32),
                9 => u64::from(x as u32),
                10 => x as i32 as i64 as u64,
                11 => x as u64,
                12 => x as i64 as u64,
                13 => f32_bits(f64::from_bits(a) as f32),
                14 => u64::from(x == y),
                15 => u64::from(x != y),
                16 => u64::from(x < y),
                17 => u64::from(x <= y),
                18 => u64::from(x > y),
                19 => u64::from(x >= y),
                _ => unreachable!(),
            });
        }
        let x = f64::from_bits(a);
        let y = f64::from_bits(b);
        return Ok(match code {
            1 => f64_bits(x + y),
            2 => f64_bits(x - y),
            3 => f64_bits(x * y),
            4 => f64_bits(x / y),
            5 => f64_bits(a as u32 as f64),
            6 => f64_bits(a as i32 as f64),
            7 => f64_bits(a as f64),
            8 => f64_bits(a as i64 as f64),
            9 => u64::from(x as u32),
            10 => x as i32 as i64 as u64,
            11 => x as u64,
            12 => x as i64 as u64,
            13 => {
                if a >> 32 != 0 {
                    return Err(STATUS_BAD_DESCRIPTOR);
                }
                f64_bits(f32::from_bits(a as u32) as f64)
            }
            14 => u64::from(x == y),
            15 => u64::from(x != y),
            16 => u64::from(x < y),
            17 => u64::from(x <= y),
            18 => u64::from(x > y),
            19 => u64::from(x >= y),
            _ => unreachable!(),
        });
    }
    if base != 0x300 {
        return Err(STATUS_UNSUPPORTED);
    }
    if !matches!(code, 12 | 25 | 27 | 29 | 30 | 31 | 32) && b != 0 {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let x = f64::from_bits(a);
    let y = f64::from_bits(b);
    Ok(f64_bits(match code {
        1 => x.sqrt(),
        2 => x.floor(),
        3 => x.ceil(),
        4 => x.trunc(),
        5 => x.round(),
        6 => x.sin(),
        7 => x.cos(),
        8 => x.tan(),
        9 => x.asin(),
        10 => x.acos(),
        11 => x.atan(),
        12 => x.atan2(y),
        13 => x.sinh(),
        14 => x.cosh(),
        15 => x.tanh(),
        16 => x.asinh(),
        17 => x.acosh(),
        18 => x.atanh(),
        19 => x.exp(),
        20 => x.exp_m1(),
        21 => x.ln(),
        22 => x.ln_1p(),
        23 => x.log2(),
        24 => x.log10(),
        25 => x.powf(y),
        26 => x.cbrt(),
        27 => x.hypot(y),
        28 => x.abs(),
        29 => x % y,
        30 => x.copysign(y),
        31 => {
            if x == 0.0 && y == 0.0 {
                f64::from_bits(a | b)
            } else {
                x.min(y)
            }
        }
        32 => {
            if x == 0.0 && y == 0.0 {
                f64::from_bits(a & b)
            } else {
                x.max(y)
            }
        }
        33 => x.round_ties_even(),
        _ => return Err(STATUS_UNSUPPORTED),
    }))
}

pub fn execute(words: &mut [u16]) -> Result<(), u16> {
    if words[3] != 0 {
        return Err(STATUS_BAD_DESCRIPTOR);
    }
    let read = |start: usize| (0..4).fold(0, |v, i| v | (u64::from(words[start + i]) << (16 * i)));
    let result = evaluate(words[2], read(4), read(8))?;
    for i in 0..4 {
        words[12 + i] = (result >> (16 * i)) as u16;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::devices::*;
    #[test]
    fn arithmetic_edges_and_integer_saturation() {
        for base in [0x100, 0x200] {
            let bits = |x: f64| {
                if base == 0x100 {
                    u64::from((x as f32).to_bits())
                } else {
                    x.to_bits()
                }
            };
            let sign = bits(-0.0);
            let nan = bits(f64::NAN);
            assert_eq!(evaluate(base + 1, sign, sign), Ok(sign));
            assert_eq!(evaluate(base + 1, sign, 0), Ok(0));
            assert_eq!(evaluate(base + 3, bits(-2.0), 0), Ok(sign));
            assert_eq!(evaluate(base + 4, bits(1.0), 0), Ok(bits(f64::INFINITY)));
            assert_eq!(evaluate(base + 4, 0, 0), Ok(nan));
            assert_eq!(
                evaluate(base + 1, bits(f64::INFINITY), bits(f64::NEG_INFINITY)),
                Ok(nan)
            );
            assert_eq!(evaluate(base + 1, 1, 1), Ok(2)); // gradual underflow
            assert_eq!(
                evaluate(base + 9, bits(f64::INFINITY), 0),
                Ok(u32::MAX as u64)
            );
            assert_eq!(
                evaluate(base + 10, bits(f64::NEG_INFINITY), 0),
                Ok(i32::MIN as i64 as u64)
            );
            assert_eq!(evaluate(base + 11, bits(f64::INFINITY), 0), Ok(u64::MAX));
            assert_eq!(
                evaluate(base + 12, bits(f64::NEG_INFINITY), 0),
                Ok(i64::MIN as u64)
            );
            for op in 9..=12 {
                assert_eq!(evaluate(base + op, nan, 0), Ok(0));
            }
            assert_eq!(evaluate(base + 9, bits(-1.0), 0), Ok(0));
            assert_eq!(evaluate(base + 10, bits(-3.9), 0), Ok(-3i64 as u64));
            assert_eq!(evaluate(base + 14, sign, 0), Ok(1));
            assert_eq!(evaluate(base + 14, nan, nan), Ok(0));
            assert_eq!(evaluate(base + 15, nan, 0), Ok(1));
            assert_eq!(evaluate(base + 7, u64::MAX, 0), Ok(bits(u64::MAX as f64)));
            assert_eq!(
                evaluate(base + 8, i64::MIN as u64, 0),
                Ok(bits(i64::MIN as f64))
            );
        }
        assert_eq!(evaluate(0x10d, (-0.0f64).to_bits(), 0), Ok(0x80000000));
        assert_eq!(
            evaluate(0x20d, 1, 0),
            Ok((f32::from_bits(1) as f64).to_bits())
        );
        // A tie rounds to the even significand.
        assert_eq!(
            evaluate(
                0x101,
                1f32.to_bits() as u64,
                (2f32.powi(-24)).to_bits() as u64
            ),
            Ok(1f32.to_bits() as u64)
        );
        assert_eq!(
            evaluate(0x201, 1f64.to_bits(), (2f64.powi(-53)).to_bits()),
            Ok(1f64.to_bits())
        );
    }
    #[test]
    fn transcendental_domain_signed_zero_and_accuracy() {
        let calc =
            |op, x: f64, y: f64| f64::from_bits(evaluate(op, x.to_bits(), y.to_bits()).unwrap());
        assert_eq!(calc(0x301, 4., 0.), 2.);
        assert_eq!(calc(0x302, -1.2, 0.), -2.);
        assert_eq!(calc(0x305, -1.5, 0.), -2.);
        assert_eq!(calc(0x306, -0., 0.).to_bits(), (-0f64).to_bits());
        assert!(calc(0x311, 0., 0.).is_nan()); // acosh
        assert!(calc(0x317, 0., 0.).is_infinite()); // log2
        assert_eq!(calc(0x319, 2., 10.), 1024.);
        assert_eq!(calc(0x31b, 3., 4.), 5.);
        for (op, x, expected) in [
            (0x311, 1., 0.),
            (0x310, 0., 0.),
            (0x312, 0., 0.),
            (0x313, 1., std::f64::consts::E),
            (0x316, 1e-15, 1e-15),
            (0x31a, 27., 3.),
        ] {
            assert!((calc(op, x, 0.) - expected).abs() < 1e-14);
        }
        for code in 1..=33 {
            assert!(evaluate(0x300 + code, 0, 0).is_ok());
        }
    }
    #[test]
    fn mailbox_is_atomic_bank_spanning_and_does_not_tick() {
        let mut state = DeviceState::default();
        let mut memory = vec![0x55aa; 256];
        let start = 63;
        let mut packet = [0; CELLS];
        packet[0] = 1;
        packet[1] = CELLS as u16;
        packet[2] = 0x201; // 1.0 + 1.0
        packet[7] = 0x3ff0;
        packet[11] = 0x3ff0;
        memory[start..start + CELLS].copy_from_slice(&packet);
        state.write_register(REGISTER_DESCRIPTOR_OFFSET, start as u16, &mut memory, 64, 4);
        state.write_register(REGISTER_COMMAND, COMMAND, &mut memory, 64, 4);
        assert_eq!(state.status(), STATUS_OK);
        assert_eq!(memory[start + 15], 0x4000);
        assert_eq!(state.tick(), 0);
        assert!(!state.has_pending_tick());
        assert_eq!(memory[start - 1], 0x55aa);
        for (index, value, status) in [
            (0, 2, STATUS_VERSION),
            (1, 15, STATUS_BAD_DESCRIPTOR),
            (2, 0xffff, STATUS_UNSUPPORTED),
            (3, 1, STATUS_BAD_DESCRIPTOR),
        ] {
            memory[start..start + CELLS].copy_from_slice(&packet);
            memory[start + index] = value;
            let before = memory.clone();
            state.write_register(REGISTER_COMMAND, COMMAND, &mut memory, 64, 4);
            assert_eq!(state.status(), status);
            assert_eq!(memory, before);
        }
        memory[250] = 1;
        memory[251] = CELLS as u16;
        state.write_register(REGISTER_DESCRIPTOR_OFFSET, 250, &mut memory, 256, 4);
        let before = memory.clone();
        state.write_register(REGISTER_COMMAND, COMMAND, &mut memory, 256, 4);
        assert_eq!(state.status(), STATUS_BOUNDS);
        assert_eq!(memory, before);
        assert_eq!(evaluate(0, 0, 0), Ok(7));
        for (op, a, b) in [
            (0, 1, 0),
            (0x101, 1 << 32, 0),
            (0x101, 0, 1 << 32),
            (0x107, 0, 1),
            (0x20d, 1 << 32, 0),
            (0x301, 0, 1),
        ] {
            assert_eq!(evaluate(op, a, b), Err(STATUS_BAD_DESCRIPTOR));
        }
    }
}
