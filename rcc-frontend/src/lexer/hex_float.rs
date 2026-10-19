//! Exact hexadecimal significand conversion, rounded once to IEEE binary32/64.
//! Syntax is validated by the lexer; unary signs are separate C tokens.
use super::FloatSuffix;

pub(super) fn bits(
    digits: &[u8],
    fractional_digits: usize,
    exponent: i128,
    suffix: FloatSuffix,
) -> u64 {
    let (precision, min_exp, max_exp, bias, infinity) = if suffix == FloatSuffix::Float {
        (24u32, -126i128, 127i128, 127i128, 0x7f80_0000u64)
    } else {
        (
            53u32,
            -1022i128,
            1023i128,
            1023i128,
            0x7ff0_0000_0000_0000u64,
        )
    };
    let Some(first) = digits.iter().position(|&d| d != 0) else {
        return 0;
    };
    let scale = 4 * (digits.len() - first - 1) as i128 - 4 * fractional_digits as i128
        + (7 - digits[first].leading_zeros()) as i128;
    let mut top_exp = exponent.saturating_add(scale);
    if top_exp > max_exp {
        return infinity;
    }
    // Below half the smallest subnormal, including enormous negative exponents.
    if top_exp < min_exp - precision as i128 {
        return 0;
    }
    let keep = (precision as i128 + (top_exp - min_exp).min(0)) as u32;
    let mut significand = 0u64;
    let mut consumed = 0u32;
    let mut started = false;
    let mut guard = false;
    let mut sticky = false;
    for &digit in &digits[first..] {
        for shift in (0..4).rev() {
            let bit = (digit >> shift) & 1;
            if !started && bit == 0 {
                continue;
            }
            started = true;
            if consumed < keep {
                significand = (significand << 1) | bit as u64;
                consumed += 1;
            } else if consumed == keep {
                guard = bit != 0;
                consumed += 1;
            } else {
                sticky |= bit != 0;
            }
        }
    }
    if consumed < keep {
        significand <<= keep - consumed;
    }
    if guard && (sticky || significand & 1 != 0) {
        significand += 1;
    }
    if top_exp < min_exp {
        // Rounding the largest subnormal up produces the smallest normal's
        // complete bit pattern too, so no special boundary case is needed.
        return significand;
    }
    if significand == 1u64 << precision {
        significand >>= 1;
        top_exp += 1;
        if top_exp > max_exp {
            return infinity;
        }
    }
    (((top_exp + bias) as u64) << (precision - 1)) | (significand & ((1u64 << (precision - 1)) - 1))
}
