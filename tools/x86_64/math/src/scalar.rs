//! Stateless IEEE scalar service shared by RVM and native applications.
//! Arithmetic rounds to nearest, ties to even. NaNs are canonicalized; integer
//! conversions saturate with NaN -> zero, matching the software runtime.
//! Adapted from rvm/src/float.rs; freestanding libm supplies transcendentals.
//! The bit/validation contract stays the same; last-bit libm equality is not promised.
const STATUS_BAD_DESCRIPTOR: u16 = 3;
const STATUS_UNSUPPORTED: u16 = 7;
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
        1 => libm::sqrt(x),
        2 => libm::floor(x),
        3 => libm::ceil(x),
        4 => libm::trunc(x),
        5 => libm::round(x),
        6 => libm::sin(x),
        7 => libm::cos(x),
        8 => libm::tan(x),
        9 => libm::asin(x),
        10 => libm::acos(x),
        11 => libm::atan(x),
        12 => libm::atan2(x, y),
        13 => libm::sinh(x),
        14 => libm::cosh(x),
        15 => libm::tanh(x),
        16 => libm::asinh(x),
        17 => libm::acosh(x),
        18 => libm::atanh(x),
        19 => libm::exp(x),
        20 => libm::expm1(x),
        21 => libm::log(x),
        22 => libm::log1p(x),
        23 => libm::log2(x),
        24 => libm::log10(x),
        25 => libm::pow(x, y),
        26 => libm::cbrt(x),
        27 => libm::hypot(x, y),
        28 => x.abs(),
        29 => libm::fmod(x, y),
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
        33 => libm::rint(x),
        _ => return Err(STATUS_UNSUPPORTED),
    }))
}
