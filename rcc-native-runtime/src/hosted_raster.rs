//! Borrowed C16 BGR/A8 spans. All validation precedes writes; no host pointers
//! escape, and page-aware row transfers preserve process/fork isolation.
use super::*;

fn blend_span(
    rt: &mut Runtime,
    at: usize,
    alpha: &[u16],
    colour: &[u16; 3],
    solid: &[u16],
    scratch: &mut [u16],
) {
    let cells = alpha.len() * 3;
    if alpha.iter().all(|&mix| mix >= 253) {
        rt.memory.write_from(at, &solid[..cells]);
        return;
    }
    let row = &mut scratch[..cells];
    rt.memory.read_into(at, row);
    for (pixel, &mix) in row.chunks_exact_mut(3).zip(alpha) {
        if mix == 0 {
            continue;
        }
        if mix >= 253 {
            pixel.copy_from_slice(colour);
        } else {
            let mix = u32::from(mix);
            for c in 0..3 {
                pixel[c] = ((u32::from(colour[c]) * mix
                    + u32::from(pixel[c]) * (255 - mix))
                    >> 8) as u16;
            }
        }
    }
    rt.memory.write_from(at, row);
}

pub(super) fn blend(rt: &mut Runtime, a: &[u32; 12]) -> Result<u32> {
    let (w, h, stride, mask_stride) = (a[2], a[3], a[4], a[5]);
    if w == 0
        || h == 0
        || w > MAX_WIDTH * 2
        || h > MAX_HEIGHT * 2
        || stride < w * 3
        || (a[1] != 0 && mask_stride < w)
        || a[6..10].iter().any(|&v| v > 255)
    {
        return Err(22);
    }
    let count = (h - 1)
        .checked_mul(stride)
        .and_then(|n| n.checked_add(w * 3))
        .ok_or(14u16)? as usize;
    let dst = span(a[0], count)?;
    if dst < BANK {
        return Err(14);
    }
    let mask = if a[1] != 0 {
        let n = (h - 1)
            .checked_mul(mask_stride)
            .and_then(|n| n.checked_add(w))
            .ok_or(14u16)? as usize;
        let start = span(a[1], n)?;
        if dst < start + n && start < dst + count {
            return Err(22);
        }
        Some(start)
    } else {
        None
    };
    // Even an entirely transparent request validates all borrowed spans above.
    // A zero mask/opacity must not read or write destination pages afterwards.
    if a[9] == 0 {
        return Ok(0);
    }
    let colour = [a[6] as u16, a[7] as u16, a[8] as u16];
    let mut solid = vec![0; w as usize * 3];
    for pixel in solid.chunks_exact_mut(3) {
        pixel.copy_from_slice(&colour);
    }
    let mut row = vec![0; solid.len()];
    let mut alpha = vec![0; w as usize];
    for y in 0..h {
        let at = dst + (y * stride) as usize;
        if let Some(mask) = mask {
            rt.memory
                .read_into(mask + (y * mask_stride) as usize, &mut alpha);
            let mut first = alpha.len();
            let mut end = 0;
            let mut active = 0;
            for (x, value) in alpha.iter_mut().enumerate() {
                let mix = if a[9] >= 253 {
                    u32::from(*value & 255)
                } else {
                    (u32::from(*value & 255) * a[9]) >> 8
                };
                *value = mix as u16;
                if mix != 0 {
                    first = first.min(x);
                    end = x + 1;
                    active += 1;
                }
            }
            if active == 0 {
                continue;
            }
            if active * 4 >= end - first {
                // Dense glyphs/images keep one page-aware row transfer. Fully
                // opaque rows copy the prepared colour without reading RAM.
                blend_span(rt, at + first * 3, &alpha[first..end], &colour, &solid, &mut row);
                continue;
            }
            // Sparse A8 curves must not round-trip the entire BGR viewport
            // merely to change a few pixels. Merge short gaps to bound page
            // lookup overhead for small antialias holes and glyph fragments.
            let mut x = first;
            while x < end {
                while x < end && alpha[x] == 0 {
                    x += 1;
                }
                if x == end {
                    break;
                }
                let start = x;
                let mut stop = x + 1;
                x += 1;
                while x < end {
                    if alpha[x] != 0 {
                        stop = x + 1;
                    } else if x - stop >= 8 {
                        break;
                    }
                    x += 1;
                }
                blend_span(rt, at + start * 3, &alpha[start..stop], &colour, &solid, &mut row);
            }
        } else if a[9] >= 253 {
            rt.memory.write_from(at, &solid);
        } else {
            rt.memory.read_into(at, &mut row);
            for pixel in row.chunks_exact_mut(3) {
                for c in 0..3 {
                    pixel[c] = ((u32::from(colour[c]) * a[9]
                        + u32::from(pixel[c]) * (255 - a[9]))
                        >> 8) as u16;
                }
            }
            rt.memory.write_from(at, &row);
        }
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::address_space::{PAGE_CELLS, PRIVATE_START};

    fn request(dst: usize, mask: usize, w: usize, h: usize, stride: usize, mask_stride: usize, opacity: u32) -> [u32; 12] {
        [crate::memory::pointer(dst) as u32,
         if mask == 0 { 0 } else { crate::memory::pointer(mask) as u32 },
         w as u32, h as u32, stride as u32, mask_stride as u32, 31, 67, 149, opacity, 0, 0]
    }

    fn expected(background: &[u16], mask: &[u16], a: &[u32; 12]) -> Vec<u16> {
        let mut result = background.to_vec();
        for y in 0..a[3] as usize {
            for x in 0..a[2] as usize {
                let mix = if a[1] == 0 {
                    if a[9] >= 253 { 255 } else { a[9] }
                } else if a[9] >= 253 {
                    u32::from(mask[y * a[5] as usize + x] & 255)
                } else {
                    (u32::from(mask[y * a[5] as usize + x] & 255) * a[9]) >> 8
                };
                for c in 0..3 {
                    let at = y * a[4] as usize + x * 3 + c;
                    result[at] = if mix == 0 { background[at] }
                        else if mix >= 253 { a[6 + c] as u16 }
                        else { ((a[6 + c] * mix + u32::from(background[at]) * (255 - mix)) >> 8) as u16 };
                }
            }
        }
        result
    }

    #[test]
    fn sparse_dense_and_transparent_spans_match_scalar_with_padding_and_high_mask_bits() {
        let mut rt = Runtime::new();
        let (dst, mask) = (3 * BANK - 7, 9 * BANK - 2);
        let mut seed = 0x472a31u32;
        for paged in [false, true] {
            if paged { rt.memory.enable_processes(); }
            for case in 0..240usize {
                seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
                let (w, h) = (1 + (seed as usize % 193), 1 + case % 7);
                let (stride, ms) = (w * 3 + case % 13, w + case % 11);
                let background: Vec<_> = (0..stride * h).map(|i| (i * 173 + case * 37) as u16).collect();
                let alpha: Vec<_> = (0..ms * h).map(|i| {
                    let value = match case % 6 {
                        0 => 0,
                        1 => 255,
                        2 if i % 67 < 3 => (i * 29) as u16 & 255,
                        2 => 0,
                        3 if i % 5 == 0 => 0,
                        3 => 255,
                        _ => (i * 71 + case * 37) as u16 & 255,
                    };
                    value | 0xa300
                }).collect();
                let opacity = [0, 1, 2, 127, 252, 253, 254, 255][case % 8];
                let a = request(dst, if case % 17 == 0 { 0 } else { mask }, w, h, stride, ms, opacity);
                rt.memory.write_from(dst, &background);
                rt.memory.write_from(mask, &alpha);
                let wanted = expected(&background, &alpha, &a);
                assert_eq!(blend(&mut rt, &a), Ok(0));
                assert_eq!(rt.memory.cells(dst..dst + background.len()).collect::<Vec<_>>(), wanted,
                           "paged {paged}, case {case}, opacity {opacity}");
                assert_eq!(rt.memory.cells(mask..mask + alpha.len()).collect::<Vec<_>>(), alpha);
            }
        }
    }

    #[test]
    fn validation_precedes_transparent_early_exit_and_all_writes() {
        let mut rt = Runtime::new();
        let (dst, mask) = (3 * BANK - 2, 5 * BANK - 1);
        let a = request(dst, mask, 17, 3, 55, 20, 0);
        let background = vec![917; 165];
        rt.memory.write_from(dst, &background);
        for (index, value, error) in [(0, 0, 14), (1, a[0], 22), (2, 0, 22),
            (4, 50, 22), (4, u32::MAX, 14), (5, 16, 22), (5, u32::MAX, 14),
            (8, 256, 22), (9, 256, 22)] {
            let mut bad = a;
            bad[index] = value;
            assert_eq!(blend(&mut rt, &bad), Err(error), "argument {index}");
            assert_eq!(rt.memory.cells(dst..dst + background.len()).collect::<Vec<_>>(), background);
        }
        let mut bad = a;
        bad[1] = crate::memory::pointer(CELLS - 4) as u32;
        assert_eq!(blend(&mut rt, &bad), Err(14));
        assert_eq!(rt.memory.cells(dst..dst + background.len()).collect::<Vec<_>>(), background);
    }

    #[test]
    fn sparse_blends_preserve_forked_pages_and_immutable_read_snapshots() {
        let mut rt = Runtime::new();
        rt.memory.enable_processes();
        for dst in [3 * BANK, PRIVATE_START + PAGE_CELLS * 3] {
            let mask = 7 * BANK;
            let (w, h, stride) = (32, 4, PAGE_CELLS);
            let background = vec![173; h * stride];
            let mut alpha = vec![0; w * h];
            alpha[w + 3] = 127;
            alpha[w + 4] = 255;
            rt.memory.write_from(dst, &background);
            rt.memory.write_from(mask, &alpha);
            let parent = rt.memory.clone();
            let snapshot = parent.read_page(dst + stride).unwrap();
            let a = request(dst, mask, w, h, stride, w, 255);
            assert_eq!(blend(&mut rt, &a), Ok(0));
            assert_eq!(parent.cells(dst..dst + background.len()).collect::<Vec<_>>(), background);
            assert!(snapshot.iter().all(|&cell| cell == 173));
            for y in [0, 2, 3] {
                assert!(Arc::ptr_eq(&parent.read_page(dst + y * stride).unwrap(),
                                   &rt.memory.read_page(dst + y * stride).unwrap()));
            }
            assert!(!Arc::ptr_eq(&snapshot, &rt.memory.read_page(dst + stride).unwrap()));
            assert_eq!(rt.memory.cells(dst..dst + background.len()).collect::<Vec<_>>(),
                       expected(&background, &alpha, &a));
        }
    }
}
