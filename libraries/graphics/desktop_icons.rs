//! Pinned Lucide coverage masks shared with Graphite, authored at both densities.
use super::policy::Rect;
include!("desktop-icons/names.rs");
const ONE: &[u8] = include_bytes!("desktop-icons/lucide-32.bin");
const TWO: &[u8] = include_bytes!("desktop-icons/lucide-64.bin");

pub fn paint(
    out: &mut [u32],
    width: u32,
    height: u32,
    scale: u32,
    x: i32,
    y: i32,
    name: &str,
    damage: Rect,
    colour: u32,
) {
    let (data, size) = match scale {
        1 => (ONE, 32usize),
        2 => (TWO, 64usize),
        _ => return,
    };
    if out.len() != width as usize * height as usize {
        return;
    }
    let index = NAMES
        .iter()
        .position(|&n| n == name)
        .or_else(|| NAMES.iter().position(|&n| n == "grid-2x2"))
        .unwrap();
    let clip = Rect::new(x, y, 32, 32)
        .intersect(damage)
        .intersect(Rect::new(
            0,
            0,
            (width / scale) as i32,
            (height / scale) as i32,
        ));
    let mask = &data[index * size * size..(index + 1) * size * size];
    for py in clip.y * scale as i32..(clip.y + clip.h) * scale as i32 {
        for px in clip.x * scale as i32..(clip.x + clip.w) * scale as i32 {
            let alpha = u32::from(
                mask[(py - y * scale as i32) as usize * size + (px - x * scale as i32) as usize],
            );
            if alpha == 0 {
                continue;
            }
            let dst = &mut out[py as usize * width as usize + px as usize];
            let mut result = 0xff000000;
            for shift in [0, 8, 16] {
                let a = (colour >> shift) & 255;
                let b = (*dst >> shift) & 255;
                result |= ((a * alpha + b * (255 - alpha) + 127) / 255) << shift;
            }
            *dst = result;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn masks_have_authored_densities_and_unknown_icons_use_fallback() {
        assert_eq!(ONE.len(), NAMES.len() * 32 * 32);
        assert_eq!(TWO.len(), NAMES.len() * 64 * 64);
        for scale in [1, 2] {
            let mut out = vec![0xff181818; (64 * 64 * scale * scale) as usize];
            let before = out.clone();
            paint(
                &mut out,
                64 * scale,
                64 * scale,
                scale,
                16,
                16,
                "unknown",
                Rect::new(0, 0, 64, 64),
                0xffffffff,
            );
            assert_ne!(out, before);
            for y in 0..64 * scale {
                for x in 0..64 * scale {
                    if !(16 * scale..48 * scale).contains(&x)
                        || !(16 * scale..48 * scale).contains(&y)
                    {
                        assert_eq!(out[(y * 64 * scale + x) as usize], 0xff181818);
                    }
                }
            }
        }
    }
}
