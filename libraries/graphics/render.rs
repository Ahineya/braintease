//! Allocation-free CPU rendering shared by hosted and future kernel backends.
//! Eight-bit A8 coverage and packed RGBA words are independent of C16 cells,
//! guest pointers, windows, operating systems and allocation policy.
//! Include this module directly from a no_std crate or a standalone rustc test.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Bounds,
    Primitive,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x: usize,
    pub y: usize,
    pub width: usize,
    pub height: usize,
}
impl Rect {
    pub fn valid(self, width: usize, height: usize) -> bool {
        self.width != 0
            && self.height != 0
            && self.x <= width
            && self.y <= height
            && self.width <= width - self.x
            && self.height <= height - self.y
    }
}

/// Memory words have bytes R,G,B,255 on every platform (not native byte order).
pub fn rgba(rgb: u32) -> u32 {
    u32::from_le_bytes([(rgb >> 16) as u8, (rgb >> 8) as u8, rgb as u8, 255])
}
pub fn rgb(pixel: u32) -> u32 {
    let c = pixel.to_le_bytes();
    (u32::from(c[0]) << 16) | (u32::from(c[1]) << 8) | u32::from(c[2])
}

/// Same integer opacity and opaque threshold as the existing LVGL raster path.
pub fn blend(background: u32, colour: u32, alpha: u8, opacity: u8) -> u32 {
    let mix = if opacity >= 253 {
        u32::from(alpha)
    } else {
        (u32::from(alpha) * u32::from(opacity)) >> 8
    };
    if mix == 0 {
        return background;
    }
    let source = rgba(colour);
    if mix >= 253 {
        return source;
    }
    let source = source.to_le_bytes();
    let old = background.to_le_bytes();
    u32::from_le_bytes([
        ((u32::from(source[0]) * mix + u32::from(old[0]) * (255 - mix)) >> 8) as u8,
        ((u32::from(source[1]) * mix + u32::from(old[1]) * (255 - mix)) >> 8) as u8,
        ((u32::from(source[2]) * mix + u32::from(old[2]) * (255 - mix)) >> 8) as u8,
        255,
    ])
}
fn layout(length: usize, width: usize, height: usize, stride: usize) -> bool {
    width != 0
        && height != 0
        && stride >= width
        && (height - 1)
            .checked_mul(stride)
            .and_then(|n| n.checked_add(width))
            .is_some_and(|n| n <= length)
}
pub fn fill_rgba(
    dst: &mut [u32],
    width: usize,
    height: usize,
    rect: Rect,
    colour: u32,
    opacity: u8,
) -> Result<(), Error> {
    if !layout(dst.len(), width, height, width) || !rect.valid(width, height) {
        return Err(Error::Bounds);
    }
    if opacity == 0 {
        return Ok(());
    }
    for y in rect.y..rect.y + rect.height {
        let row = &mut dst[y * width + rect.x..y * width + rect.x + rect.width];
        if opacity >= 253 {
            row.fill(rgba(colour));
        } else {
            for pixel in row {
                // An unmasked fill uses opacity directly. Multiplying it by
                // an artificial 255 mask would lower the effective opacity.
                *pixel = blend(*pixel, colour, opacity, 255);
            }
        }
    }
    Ok(())
}
pub fn mask_rgba(
    dst: &mut [u32],
    width: usize,
    height: usize,
    rect: Rect,
    mask: &[u8],
    stride: usize,
    colour: u32,
    opacity: u8,
) -> Result<(), Error> {
    if !layout(dst.len(), width, height, width)
        || !rect.valid(width, height)
        || !layout(mask.len(), rect.width, rect.height, stride)
    {
        return Err(Error::Bounds);
    }
    if opacity == 0 {
        return Ok(());
    }
    for y in 0..rect.height {
        let row =
            &mut dst[(rect.y + y) * width + rect.x..(rect.y + y) * width + rect.x + rect.width];
        for (pixel, &alpha) in row
            .iter_mut()
            .zip(&mask[y * stride..y * stride + rect.width])
        {
            if alpha != 0 {
                *pixel = blend(*pixel, colour, alpha, opacity);
            }
        }
    }
    Ok(())
}

/// Move an in-place rectangle with snapshot-equivalent overlap semantics.
/// Row order protects vertical overlap; slice copy_within protects horizontal
/// overlap, so no allocator or temporary full-frame buffer is required.
pub fn copy_rgba(
    dst: &mut [u32],
    width: usize,
    height: usize,
    source: Rect,
    x: usize,
    y: usize,
) -> Result<(), Error> {
    let target = Rect {
        x,
        y,
        width: source.width,
        height: source.height,
    };
    if !layout(dst.len(), width, height, width)
        || !source.valid(width, height)
        || !target.valid(width, height)
    {
        return Err(Error::Bounds);
    }
    if y > source.y {
        for row in (0..source.height).rev() {
            let at = (source.y + row) * width + source.x;
            dst.copy_within(at..at + source.width, (y + row) * width + x);
        }
    } else {
        for row in 0..source.height {
            let at = (source.y + row) * width + source.x;
            dst.copy_within(at..at + source.width, (y + row) * width + x);
        }
    }
    Ok(())
}

fn floor(value: f64) -> i32 {
    let n = value as i32;
    if value < f64::from(n) {
        n - 1
    } else {
        n
    }
}
fn ceil(value: f64) -> i32 {
    let n = value as i32;
    if value > f64::from(n) {
        n + 1
    } else {
        n
    }
}
const FIXED_ONE: i32 = 65536;

#[derive(Clone, Copy)]
pub struct Primitive {
    pub x0: i32,
    pub y0: i32,
    pub x1: i32,
    pub y1: i32,
    pub dot: bool,
    pub size: u16,
}

struct Coverage<'a> {
    width: usize,
    height: usize,
    stride: usize,
    pixels: &'a mut [u8],
}
impl Coverage<'_> {
    #[inline]
    fn cover(&mut self, x: i32, y: i32, value: u8) {
        if value != 0 && x >= 0 && y >= 0 && (x as usize) < self.width && (y as usize) < self.height
        {
            let at = y as usize * self.stride + x as usize;
            self.pixels[at] = self.pixels[at].max(value);
        }
    }

    fn line(&mut self, p: Primitive) {
        let (mut ax, mut ay) = (f64::from(p.x0), f64::from(p.y0));
        let (dx, dy) = (f64::from(p.x1) - ax, f64::from(p.y1) - ay);
        let stroke = i32::from(p.size);
        if dx == 0.0 && dy == 0.0 {
            if ax < -f64::from(stroke)
                || ay < -f64::from(stroke)
                || ax > self.width as f64 + f64::from(stroke)
                || ay > self.height as f64 + f64::from(stroke)
            {
                return;
            }
            for y in 0..stroke {
                for x in 0..stroke {
                    self.cover(p.x0 - stroke / 2 + x, p.y0 - stroke / 2 + y, 255);
                }
            }
            return;
        }
        let steep = dy.abs() > dx.abs();
        let halo = f64::from(stroke) + 1.0;
        let (mut lo, mut hi) = (0.0, 1.0);
        if !clip(-dx, ax + halo, &mut lo, &mut hi)
            || !clip(dx, self.width as f64 - 1.0 + halo - ax, &mut lo, &mut hi)
            || !clip(-dy, ay + halo, &mut lo, &mut hi)
            || !clip(dy, self.height as f64 - 1.0 + halo - ay, &mut lo, &mut hi)
        {
            return;
        }
        let (mut bx, mut by) = (ax + hi * dx, ay + hi * dy);
        ax += lo * dx;
        ay += lo * dy;
        if if steep { ay > by } else { ax > bx } {
            core::mem::swap(&mut ax, &mut bx);
            core::mem::swap(&mut ay, &mut by);
        }
        let first = if steep { ay } else { ax };
        let last = if steep { by } else { bx };
        let (start, end) = (ceil(first), floor(last));
        let slope = if steep { dx / dy } else { dy / dx };
        let minor = if steep { ax } else { ay } + (f64::from(start) - first) * slope;
        let mut fixed = nearest(minor * f64::from(FIXED_ONE));
        let step = nearest(slope * f64::from(FIXED_ONE));
        for major in start..=end {
            let base = fixed.div_euclid(FIXED_ONE);
            let fraction = fixed.rem_euclid(FIXED_ONE);
            let low = base - stroke / 2;
            let upper = ((fraction * 255 + 32768) / FIXED_ONE) as u8;
            if steep {
                self.cover(low, major, 255 - upper);
            } else {
                self.cover(major, low, 255 - upper);
            }
            for n in 1..stroke {
                if steep {
                    self.cover(low + n, major, 255);
                } else {
                    self.cover(major, low + n, 255);
                }
            }
            if steep {
                self.cover(low + stroke, major, upper);
            } else {
                self.cover(major, low + stroke, upper);
            }
            fixed += step;
        }
    }

    fn dot(&mut self, p: Primitive) {
        let r = i32::from(p.size);
        if p.x0 < -r
            || p.y0 < -r
            || p.x0 > self.width as i32 - 1 + r
            || p.y0 > self.height as i32 - 1 + r
        {
            return;
        }
        let (inner, outer) = ((r * r) as u32, ((r + 1) * (r + 1)) as u32);
        for dy in 0..=r {
            for dx in 0..=r {
                let distance = (dx * dx + dy * dy) as u32;
                let coverage = if distance <= inner {
                    255
                } else if distance >= outer {
                    continue;
                } else {
                    let edge = (r as u32 + 1) * 256 - square_root(distance << 16);
                    ((edge * 255 + 128) / 256) as u8
                };
                self.cover(p.x0 + dx, p.y0 + dy, coverage);
                if dx != 0 {
                    self.cover(p.x0 - dx, p.y0 + dy, coverage);
                }
                if dy != 0 {
                    self.cover(p.x0 + dx, p.y0 - dy, coverage);
                    if dx != 0 {
                        self.cover(p.x0 - dx, p.y0 - dy, coverage);
                    }
                }
            }
        }
    }
}

fn clip(direction: f64, distance: f64, lo: &mut f64, hi: &mut f64) -> bool {
    if direction == 0.0 {
        return distance >= 0.0;
    }
    let t = distance / direction;
    if direction < 0.0 {
        if t > *hi {
            return false;
        }
        if t > *lo {
            *lo = t;
        }
    } else {
        if t < *lo {
            return false;
        }
        if t < *hi {
            *hi = t;
        }
    }
    true
}
fn nearest(x: f64) -> i32 {
    (if x < 0.0 { x - 0.5 } else { x + 0.5 }) as i32
}
fn square_root(mut value: u32) -> u32 {
    let (mut root, mut bit) = (0, 1u32 << 22);
    while bit > value {
        bit >>= 2;
    }
    while bit != 0 {
        if value >= root + bit {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    root
}

impl Primitive {
    pub fn valid(self) -> bool {
        self.size <= 8 && (self.dot || self.size != 0)
    }
}
/// Validate the entire command list before touching pixels, including padding.
pub fn raster_a8(
    dst: &mut [u8],
    width: usize,
    height: usize,
    stride: usize,
    primitives: &[Primitive],
    clear: bool,
) -> Result<(), Error> {
    if width > 8192 || height > 8192 || !layout(dst.len(), width, height, stride) {
        return Err(Error::Bounds);
    }
    if primitives.iter().any(|p| !p.valid()) {
        return Err(Error::Primitive);
    }
    let mut coverage = Coverage {
        width,
        height,
        stride,
        pixels: dst,
    };
    if clear {
        for y in 0..height {
            coverage.pixels[y * stride..y * stride + width].fill(0);
        }
    }
    for &p in primitives {
        if p.dot {
            coverage.dot(p);
        } else {
            coverage.line(p);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rectangle_copy_matches_snapshot_for_every_overlap_direction() {
        let original: [u32; 64] = core::array::from_fn(|n| n as u32);
        for sx in 0..5 {
            for sy in 0..5 {
                for dx in 0..5 {
                    for dy in 0..5 {
                        let mut actual = original;
                        let mut wanted = original;
                        let source = Rect {
                            x: sx,
                            y: sy,
                            width: 4,
                            height: 4,
                        };
                        for row in 0..4 {
                            for col in 0..4 {
                                wanted[(dy + row) * 8 + dx + col] =
                                    original[(sy + row) * 8 + sx + col];
                            }
                        }
                        copy_rgba(&mut actual, 8, 8, source, dx, dy).unwrap();
                        assert_eq!(actual, wanted);
                    }
                }
            }
        }
        let mut actual = original;
        assert_eq!(
            copy_rgba(
                &mut actual,
                8,
                8,
                Rect {
                    x: 7,
                    y: 0,
                    width: 2,
                    height: 2
                },
                0,
                0
            ),
            Err(Error::Bounds)
        );
        assert_eq!(actual, original);
    }
    #[test]
    fn unmasked_fill_uses_opacity_directly_for_every_channel_value() {
        let rect = Rect {
            x: 0,
            y: 0,
            width: 1,
            height: 1,
        };
        for opacity in 0..=255u32 {
            for channel in 0..=255u32 {
                let old = rgba((channel << 16) | ((255 - channel) << 8) | 149);
                let colour = (113 << 16) | (channel << 8) | (255 - channel);
                let mut actual = [old];
                fill_rgba(&mut actual, 1, 1, rect, colour, opacity as u8).unwrap();
                let expected = if opacity == 0 {
                    old
                } else if opacity >= 253 {
                    rgba(colour)
                } else {
                    let source = rgba(colour).to_le_bytes();
                    let previous = old.to_le_bytes();
                    u32::from_le_bytes([
                        ((u32::from(source[0]) * opacity
                            + u32::from(previous[0]) * (255 - opacity))
                            >> 8) as u8,
                        ((u32::from(source[1]) * opacity
                            + u32::from(previous[1]) * (255 - opacity))
                            >> 8) as u8,
                        ((u32::from(source[2]) * opacity
                            + u32::from(previous[2]) * (255 - opacity))
                            >> 8) as u8,
                        255,
                    ])
                };
                assert_eq!(actual[0], expected, "opacity {opacity}, channel {channel}");
            }
        }
    }
    #[test]
    fn alpha_matches_the_existing_opaque_threshold_and_integer_rounding() {
        let old = rgba(0x315e9b);
        let colour = 0x8f26cd;
        for opacity in 0..=255u32 {
            for alpha in 0..=255u32 {
                let mix = if opacity >= 253 {
                    alpha
                } else {
                    alpha * opacity >> 8
                };
                let expected = if mix == 0 {
                    old
                } else if mix >= 253 {
                    rgba(colour)
                } else {
                    let source = rgba(colour).to_le_bytes();
                    let previous = old.to_le_bytes();
                    u32::from_le_bytes([
                        ((u32::from(source[0]) * mix + u32::from(previous[0]) * (255 - mix)) >> 8)
                            as u8,
                        ((u32::from(source[1]) * mix + u32::from(previous[1]) * (255 - mix)) >> 8)
                            as u8,
                        ((u32::from(source[2]) * mix + u32::from(previous[2]) * (255 - mix)) >> 8)
                            as u8,
                        255,
                    ])
                };
                assert_eq!(blend(old, colour, alpha as u8, opacity as u8), expected);
            }
        }
    }
    #[test]
    fn raster_preserves_padding_and_rejects_late_invalid_primitive_atomically() {
        let initial = [87u8; 79];
        let mut pixels = initial;
        let good = Primitive {
            x0: i32::MIN,
            y0: 0,
            x1: i32::MAX,
            y1: 6,
            dot: false,
            size: 2,
        };
        let bad = Primitive { size: 9, ..good };
        assert_eq!(
            raster_a8(&mut pixels, 9, 7, 11, &[good, bad], true),
            Err(Error::Primitive)
        );
        assert_eq!(pixels, initial);
        raster_a8(&mut pixels, 9, 7, 11, &[good], true).unwrap();
        for row in 0..7 {
            assert_eq!(&pixels[row * 11 + 9..row * 11 + 11], &[87, 87]);
        }
        let previous = pixels;
        raster_a8(&mut pixels, 9, 7, 11, &[good], false).unwrap();
        assert_eq!(pixels, previous);
        assert_eq!(&pixels[77..], &[87, 87]);
    }
    #[test]
    fn invalid_colour_and_mask_rectangles_are_atomic() {
        let initial = [rgba(0x102030); 20];
        let mut pixels = initial;
        let rect = Rect {
            x: 3,
            y: 1,
            width: 2,
            height: 2,
        };
        assert_eq!(
            fill_rgba(&mut pixels, 4, 5, rect, 0xffffff, 255),
            Err(Error::Bounds)
        );
        assert_eq!(pixels, initial);
        let rect = Rect {
            x: 1,
            y: 1,
            width: 2,
            height: 2,
        };
        assert_eq!(
            mask_rgba(&mut pixels, 4, 5, rect, &[255; 3], 2, 0xffffff, 255),
            Err(Error::Bounds)
        );
        assert_eq!(pixels, initial);
        mask_rgba(
            &mut pixels,
            4,
            5,
            rect,
            &[255, 0, 127, 0, 255],
            3,
            0xffffff,
            255,
        )
        .unwrap();
        assert_eq!(pixels[5], rgba(0xffffff));
        assert_eq!(pixels[6], initial[6]);
        assert_eq!(pixels[9], initial[9]);
        assert_eq!(pixels[10], rgba(0xffffff));
    }
}
