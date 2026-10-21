//! Graphite's pre-rasterized Noto Sans labels, independent of any toolkit/OS.
//! The 1x/2x tables are the same licensed A8 metrics used by Graphite apps.
//! No allocation, shaping engine, runtime font service, or mask rescaling.
use super::policy::Rect;
const ONE: &[u8] = include_bytes!("desktop-fonts/noto-12.bin");
const TWO: &[u8] = include_bytes!("desktop-fonts/noto-24.bin");
const HEADER: usize = 12;
const RECORD: usize = 18;
fn u16_at(data: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([data[at], data[at + 1]])
}
fn u32_at(data: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([data[at], data[at + 1], data[at + 2], data[at + 3]])
}
#[derive(Clone, Copy)]
struct Glyph {
    index: usize,
    advance: u32,
    width: i32,
    height: i32,
    x: i32,
    y: i32,
}
#[derive(Clone, Copy)]
struct Font {
    data: &'static [u8],
    count: usize,
    height: i32,
    baseline: i32,
    pixels: usize,
}
impl Font {
    fn at_scale(scale: u32) -> Option<Self> {
        let data = match scale {
            1 => ONE,
            2 => TWO,
            _ => return None,
        };
        let count = u32_at(data, 4) as usize;
        Some(Self {
            data,
            count,
            height: u16_at(data, 8) as i32,
            baseline: u16_at(data, 10) as i32,
            pixels: HEADER + count * RECORD,
        })
    }
    fn lookup(self, cp: u32) -> Option<Glyph> {
        let (mut lo, mut hi) = (0, self.count);
        while lo < hi {
            let mid = (lo + hi) / 2;
            let at = HEADER + mid * RECORD;
            match u32_at(self.data, at).cmp(&cp) {
                core::cmp::Ordering::Less => lo = mid + 1,
                core::cmp::Ordering::Greater => hi = mid,
                core::cmp::Ordering::Equal => {
                    return Some(Glyph {
                        index: u32_at(self.data, at + 4) as usize,
                        advance: u16_at(self.data, at + 8) as u32,
                        width: u16_at(self.data, at + 10) as i32,
                        height: u16_at(self.data, at + 12) as i32,
                        x: u16_at(self.data, at + 14) as i16 as i32,
                        y: u16_at(self.data, at + 16) as i16 as i32,
                    })
                }
            }
        }
        None
    }
    fn glyph(self, ch: char) -> Glyph {
        self.lookup(ch as u32)
            .or_else(|| self.lookup('?' as u32))
            .unwrap()
    }
    fn advance(self, text: &str) -> u32 {
        text.chars()
            .fold(0u32, |n, c| n.saturating_add(self.glyph(c).advance))
    }
}
/// Physical width, using source metrics at the selected density.
pub fn measure(text: &str, scale: u32) -> u32 {
    Font::at_scale(scale).map_or(0, |font| font.advance(text).div_ceil(16))
}
/// Paint an ellipsized label into an owned packed RGBA frame. The destination
/// and damage rectangles count logical pixels; source masks already match DPI.
pub fn paint(
    out: &mut [u32],
    width: u32,
    height: u32,
    scale: u32,
    r: Rect,
    damage: Rect,
    text: &str,
    colour: u32,
    center: bool,
) {
    let Some(font) = Font::at_scale(scale) else {
        return;
    };
    if out.len() != width as usize * height as usize || r.empty() {
        return;
    }
    let clipped = r.intersect(damage).intersect(Rect::new(
        0,
        0,
        (width / scale) as i32,
        (height / scale) as i32,
    ));
    if clipped.empty() {
        return;
    }
    let clip = Rect::new(
        clipped.x * scale as i32,
        clipped.y * scale as i32,
        clipped.w * scale as i32,
        clipped.h * scale as i32,
    );
    let max_width = (r.w as u32 * scale) * 16;
    let advance = measure(text, scale) * 16;
    let ellipsis = font.glyph('…');
    let shortened = advance > max_width;
    let budget = if shortened {
        max_width.saturating_sub(ellipsis.advance)
    } else {
        max_width
    };
    let mut cursor = if center && !shortened {
        (max_width - advance) / 2
    } else {
        0
    };
    let start = cursor;
    let baseline =
        r.y * scale as i32 + (r.h * scale as i32 - font.height) / 2 + font.height - font.baseline;
    for ch in text.chars() {
        let g = font.glyph(ch);
        if cursor - start + g.advance > budget {
            break;
        }
        draw(
            out,
            width,
            &font,
            g,
            r.x * scale as i32 + (cursor / 16) as i32,
            baseline,
            clip,
            colour,
        );
        cursor += g.advance;
    }
    if shortened && max_width >= ellipsis.advance {
        draw(
            out,
            width,
            &font,
            ellipsis,
            r.x * scale as i32 + (cursor / 16) as i32,
            baseline,
            clip,
            colour,
        );
    }
}
fn draw(
    out: &mut [u32],
    stride: u32,
    font: &Font,
    g: Glyph,
    x: i32,
    baseline: i32,
    clip: Rect,
    colour: u32,
) {
    let left = x + g.x;
    let top = baseline - g.height - g.y;
    let region = Rect::new(left, top, g.width, g.height).intersect(clip);
    if region.empty() {
        return;
    }
    let foreground = colour.to_le_bytes();
    for y in region.y..region.y + region.h {
        for x in region.x..region.x + region.w {
            let alpha =
                font.data[font.pixels + g.index + ((y - top) * g.width + x - left) as usize] as u32;
            if alpha == 0 {
                continue;
            }
            let at = (y as u32 * stride + x as u32) as usize;
            let background = out[at].to_le_bytes();
            let blend = |i: usize| {
                ((foreground[i] as u32 * alpha + background[i] as u32 * (255 - alpha) + 127) / 255)
                    as u8
            };
            out[at] = u32::from_le_bytes([blend(0), blend(1), blend(2), 255]);
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn asset_bounds_metrics_unicode_and_fallback() {
        for scale in [1, 2] {
            let f = Font::at_scale(scale).unwrap();
            assert_eq!(&f.data[..4], b"GDF1");
            for i in 0..f.count {
                let cp = u32_at(f.data, HEADER + i * RECORD);
                let g = f.lookup(cp).unwrap();
                assert!(f.pixels + g.index + (g.width * g.height) as usize <= f.data.len());
            }
            for c in "CalculatorУкраїнські Ґґ Єє Іі Її café…".chars() {
                assert!(f.lookup(c as u32).is_some());
            }
            assert_eq!(f.glyph('🦀').index, f.glyph('?').index);
            assert_ne!(f.glyph('A').index, f.glyph('a').index);
        }
        assert_eq!(Font::at_scale(3).map(|f| f.height), None);
    }
    #[test]
    fn mixed_case_and_ukrainian_are_crisp_at_both_densities() {
        for scale in [1, 2] {
            let (w, h) = (300 * scale, 28 * scale);
            let mut out = vec![0xff181818; (w * h) as usize];
            paint(
                &mut out,
                w,
                h,
                scale,
                Rect::new(4, 0, 290, 28),
                Rect::new(0, 0, 300, 28),
                "Calculator — Українські Ґґ Єє Іі Її",
                0xffdddddd,
                false,
            );
            assert!(out.iter().filter(|&&p| p != 0xff181818).count() > 200 * scale as usize);
            assert!(out.iter().any(|&p| p != 0xff181818 && p != 0xffdddddd));
            assert!(out.iter().all(|p| p >> 24 == 255));
        }
        assert!(measure("Calculator", 2).abs_diff(2 * measure("Calculator", 1)) <= 3);
    }
    #[test]
    fn damage_and_ellipsis_preserve_every_outside_pixel() {
        for scale in [1, 2] {
            let (w, h) = (120 * scale, 30 * scale);
            let base = 0xff101010;
            let mut out = vec![base; (w * h) as usize];
            let r = Rect::new(10, 4, 45, 20);
            let damage = Rect::new(15, 0, 20, 30);
            paint(
                &mut out,
                w,
                h,
                scale,
                r,
                damage,
                "Long mixed-case filename.txt",
                0xfffafafa,
                false,
            );
            for y in 0..h {
                for x in 0..w {
                    if !r
                        .intersect(damage)
                        .contains((x / scale) as i32, (y / scale) as i32)
                    {
                        assert_eq!(out[(y * w + x) as usize], base);
                    }
                }
            }
            assert!(out.iter().any(|&p| p != base));
        }
    }
}
