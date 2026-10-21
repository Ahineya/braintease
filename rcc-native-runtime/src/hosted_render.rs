//! Owned retained targets. Guest adapters borrow and snapshot cells only at the
//! boundary; the render core stores conventional bytes and knows no guest ABI.
use super::*;
#[path = "../../libraries/graphics/render.rs"]
pub(super) mod core;
use core::{Primitive, Rect};

const A8: u32 = 3;
const XRGB8888: u32 = 2;
const MAX_TARGETS: usize = 32;
const MAX_BYTES: usize = 64 * 1024 * 1024;
const MAX_COMMANDS: usize = 256;
const MAX_DATA_CELLS: usize = 8 * 1024 * 1024;
const WORDS_PER_COMMAND: usize = 12;
const MAX_PRIMITIVES: usize = 8192;

enum Pixels {
    A8(Vec<u8>),
    Colour(Vec<u32>),
}
struct Target {
    width: usize,
    height: usize,
    pixels: Pixels,
}
impl Target {
    fn format(&self) -> u32 {
        match self.pixels {
            Pixels::A8(_) => A8,
            Pixels::Colour(_) => XRGB8888,
        }
    }
    fn bytes(&self) -> usize {
        self.width * self.height * if self.format() == A8 { 1 } else { 4 }
    }
    fn rect(&self, x: u32, y: u32, width: u32, height: u32) -> Result<Rect> {
        let r = Rect {
            x: x as usize,
            y: y as usize,
            width: width as usize,
            height: height as usize,
        };
        if !r.valid(self.width, self.height) {
            return Err(22);
        }
        Ok(r)
    }
}
#[derive(Default)]
pub(super) struct State {
    next: u32,
    bytes: usize,
    targets: BTreeMap<u32, Target>,
}
impl State {
    fn target(&self, handle: u32) -> Result<&Target> {
        self.targets.get(&handle).ok_or(22)
    }
    fn target_mut(&mut self, handle: u32) -> Result<&mut Target> {
        self.targets.get_mut(&handle).ok_or(22)
    }
    fn create(&mut self, width: u32, height: u32, format: u32) -> Result<u32> {
        if width == 0
            || height == 0
            || width > MAX_WIDTH * 2
            || height > MAX_HEIGHT * 2
            || !matches!(format, A8 | XRGB8888)
        {
            return Err(22);
        }
        let count = width as usize * height as usize;
        let bytes = count * if format == A8 { 1 } else { 4 };
        if self.targets.len() >= MAX_TARGETS || bytes > MAX_BYTES - self.bytes {
            return Err(12);
        }
        let handle = self.next.checked_add(1).ok_or(12u16)?;
        let pixels = if format == A8 {
            Pixels::A8(vec![0; count])
        } else {
            Pixels::Colour(vec![core::rgba(0); count])
        };
        self.targets.insert(
            handle,
            Target {
                width: width as usize,
                height: height as usize,
                pixels,
            },
        );
        self.next = handle;
        self.bytes += bytes;
        Ok(handle)
    }
    fn destroy(&mut self, handle: u32) -> Result<()> {
        let target = self.targets.remove(&handle).ok_or(22u16)?;
        self.bytes -= target.bytes();
        Ok(())
    }
}

fn words(rt: &Runtime, ptr: u32, count: usize) -> Result<Vec<u32>> {
    let start = span(ptr, count.checked_mul(2).ok_or(14u16)?)?;
    if start < BANK {
        return Err(14);
    }
    let mut cells = vec![0; count * 2];
    rt.memory.read_into(start, &mut cells);
    Ok(cells
        .chunks_exact(2)
        .map(|n| u32::from(n[0]) | (u32::from(n[1]) << 16))
        .collect())
}
fn bytes(rt: &Runtime, ptr: u32, count: usize) -> Result<Vec<u8>> {
    let start = span(ptr, count)?;
    if start < BANK {
        return Err(14);
    }
    let mut cells = vec![0; count];
    rt.memory.read_into(start, &mut cells);
    Ok(cells.into_iter().map(|n| n as u8).collect())
}
fn data_span(
    offset: u32,
    width: usize,
    height: usize,
    stride: usize,
    length: usize,
) -> Result<std::ops::Range<usize>> {
    if stride < width {
        return Err(22);
    }
    let start = offset as usize;
    let end = (height - 1)
        .checked_mul(stride)
        .and_then(|n| n.checked_add(width))
        .and_then(|n| n.checked_add(start))
        .ok_or(22u16)?;
    if end > length {
        return Err(22);
    }
    Ok(start..end)
}

enum Command {
    Fill(Rect, u32, u8),
    Mask(Rect, u32, u8, Vec<u8>),
    Copy(Rect, Vec<u32>),
    CopySelf(Rect, Rect),
}
impl State {
    fn batch(
        &mut self,
        rt: &Runtime,
        handle: u32,
        command_ptr: u32,
        count: u32,
        data_ptr: u32,
        data_cells: u32,
    ) -> Result<()> {
        if count as usize > MAX_COMMANDS || data_cells as usize > MAX_DATA_CELLS {
            return Err(22);
        }
        let target = self.target(handle)?;
        if target.format() != XRGB8888 {
            return Err(22);
        }
        let packed = if count == 0 {
            Vec::new()
        } else {
            words(rt, command_ptr, count as usize * WORDS_PER_COMMAND)?
        };
        let data = if data_cells == 0 {
            Vec::new()
        } else {
            bytes(rt, data_ptr, data_cells as usize)?
        };
        let mut commands = Vec::with_capacity(count as usize);
        let mut staging = 0usize;
        for c in packed.chunks_exact(WORDS_PER_COMMAND) {
            let rect = target.rect(c[1], c[2], c[3], c[4])?;
            if c[5] > 0xffffff || c[6] > 255 || c[10] != 0 || c[11] != 0 {
                return Err(22);
            }
            let staging_bytes = rect.width
                * rect.height
                * match c[0] {
                    1 => 0,
                    4 if c[7] == handle => 0,
                    4 => 4,
                    _ => 1,
                };
            staging = staging.checked_add(staging_bytes).ok_or(12u16)?;
            if staging > MAX_BYTES {
                return Err(12);
            }
            commands.push(match c[0] {
                1 => {
                    if c[7..10].iter().any(|&n| n != 0) {
                        return Err(22);
                    }
                    Command::Fill(rect, c[5], c[6] as u8)
                }
                2 => {
                    if c[9] != 0 {
                        return Err(22);
                    }
                    let range =
                        data_span(c[7], rect.width, rect.height, c[8] as usize, data.len())?;
                    let mut mask = Vec::with_capacity(rect.width * rect.height);
                    for row in 0..rect.height {
                        let at = range.start + row * c[8] as usize;
                        mask.extend_from_slice(&data[at..at + rect.width]);
                    }
                    Command::Mask(rect, c[5], c[6] as u8, mask)
                }
                3 => {
                    let source = self.target(c[7])?;
                    let r = source.rect(c[8], c[9], c[3], c[4])?;
                    let Pixels::A8(source) = &source.pixels else {
                        return Err(22);
                    };
                    let source_width = self.target(c[7])?.width;
                    let mut mask = Vec::with_capacity(rect.width * rect.height);
                    for row in 0..rect.height {
                        let at = (r.y + row) * source_width + r.x;
                        mask.extend_from_slice(&source[at..at + rect.width]);
                    }
                    Command::Mask(rect, c[5], c[6] as u8, mask)
                }
                4 => {
                    if c[5] != 0 || c[6] != 0 {
                        return Err(22);
                    }
                    let source = self.target(c[7])?;
                    let r = source.rect(c[8], c[9], c[3], c[4])?;
                    let Pixels::Colour(pixels) = &source.pixels else {
                        return Err(22);
                    };
                    if c[7] == handle {
                        Command::CopySelf(rect, r)
                    } else {
                        let mut copy = Vec::with_capacity(rect.width * rect.height);
                        for row in 0..rect.height {
                            let at = (r.y + row) * source.width + r.x;
                            copy.extend_from_slice(&pixels[at..at + rect.width]);
                        }
                        Command::Copy(rect, copy)
                    }
                }
                _ => return Err(22),
            });
        }
        let target = self.target_mut(handle)?;
        let Pixels::Colour(pixels) = &mut target.pixels else {
            unreachable!()
        };
        for command in commands {
            match command {
                Command::Fill(r, rgb, opacity) => {
                    core::fill_rgba(pixels, target.width, target.height, r, rgb, opacity)
                        .expect("validated rectangle")
                }
                Command::Mask(r, rgb, opacity, mask) => core::mask_rgba(
                    pixels,
                    target.width,
                    target.height,
                    r,
                    &mask,
                    r.width,
                    rgb,
                    opacity,
                )
                .expect("validated mask"),
                Command::Copy(r, copy) => {
                    for y in 0..r.height {
                        let at = (r.y + y) * target.width + r.x;
                        pixels[at..at + r.width]
                            .copy_from_slice(&copy[y * r.width..(y + 1) * r.width]);
                    }
                }
                Command::CopySelf(r, source) => {
                    core::copy_rgba(pixels, target.width, target.height, source, r.x, r.y)
                        .expect("validated rectangle")
                }
            }
        }
        Ok(())
    }
    fn raster(
        &mut self,
        rt: &Runtime,
        handle: u32,
        ptr: u32,
        count: u32,
        flags: u32,
    ) -> Result<()> {
        let target = self.target(handle)?;
        if target.format() != A8 || count as usize > MAX_PRIMITIVES || flags & !1 != 0 {
            return Err(22);
        }
        let packed = if count == 0 {
            Vec::new()
        } else {
            words(rt, ptr, count as usize * 6)?
        };
        let mut primitives = Vec::with_capacity(count as usize);
        for p in packed.chunks_exact(6) {
            if p[4] > 1 || p[5] > 8 || (p[4] == 0 && p[5] == 0) {
                return Err(22);
            }
            primitives.push(Primitive {
                x0: p[0] as i32,
                y0: p[1] as i32,
                x1: p[2] as i32,
                y1: p[3] as i32,
                dot: p[4] == 1,
                size: p[5] as u16,
            });
        }
        let target = self.target_mut(handle)?;
        let Pixels::A8(pixels) = &mut target.pixels else {
            unreachable!()
        };
        core::raster_a8(
            pixels,
            target.width,
            target.height,
            target.width,
            &primitives,
            flags & 1 != 0,
        )
        .map_err(|_| 22)
    }
}

impl State {
    fn transfer(&mut self, rt: &mut Runtime, a: &[u32; 12], upload: bool) -> Result<()> {
        let target = self.target(a[2])?;
        let rect = target.rect(a[3], a[4], a[5], a[6])?;
        let channels = if target.format() == A8 { 1 } else { 3 };
        let width = rect.width * channels;
        let range = data_span(0, width, rect.height, a[7] as usize, a[9] as usize)?;
        if a[9] as usize > MAX_DATA_CELLS || a[10] != 0 || a[11] != 0 {
            return Err(22);
        }
        // The complete advertised span is validated even when only its rectangle
        // is transferred. Its padding and excess cells are never overwritten.
        let at = span(a[8], a[9] as usize)?;
        if at < BANK {
            return Err(14);
        }
        let mut row = vec![0u16; width];
        if upload {
            let mut upload = Vec::with_capacity(width * rect.height);
            for y in 0..rect.height {
                rt.memory.read_into(at + y * a[7] as usize, &mut row);
                upload.extend(row.iter().map(|&n| n as u8));
            }
            let target = self.target_mut(a[2])?;
            for y in 0..rect.height {
                let base = (rect.y + y) * target.width + rect.x;
                let input = &upload[y * width..(y + 1) * width];
                match &mut target.pixels {
                    Pixels::A8(pixels) => pixels[base..base + rect.width].copy_from_slice(input),
                    Pixels::Colour(pixels) => {
                        for (out, c) in pixels[base..base + rect.width]
                            .iter_mut()
                            .zip(input.chunks_exact(3))
                        {
                            *out = core::rgba(
                                (u32::from(c[2]) << 16) | (u32::from(c[1]) << 8) | u32::from(c[0]),
                            );
                        }
                    }
                }
            }
        } else {
            for y in 0..rect.height {
                let base = (rect.y + y) * target.width + rect.x;
                match &target.pixels {
                    Pixels::A8(pixels) => {
                        for (out, &n) in row.iter_mut().zip(&pixels[base..base + rect.width]) {
                            *out = u16::from(n);
                        }
                    }
                    Pixels::Colour(pixels) => {
                        for (out, &n) in row
                            .chunks_exact_mut(3)
                            .zip(&pixels[base..base + rect.width])
                        {
                            let c = n.to_le_bytes();
                            out.copy_from_slice(&[
                                u16::from(c[2]),
                                u16::from(c[1]),
                                u16::from(c[0]),
                            ]);
                        }
                    }
                }
                rt.memory.write_from(at + y * a[7] as usize, &row);
            }
        }
        let _ = range;
        Ok(())
    }
}

/// The only C16 adapter: all words/pointers disappear before entering the core.
pub(super) fn call(rt: &mut Runtime, window: &mut Surface, a: &mut [u32; 12]) -> Result<u32> {
    match a[1] {
        1 => {
            if a[2] != 0 || a[6..].iter().any(|&n| n != 0) {
                return Err(22);
            }
            a[2] = window.render.create(a[3], a[4], a[5])?;
        }
        2 => {
            if a[3..].iter().any(|&n| n != 0) {
                return Err(22);
            }
            window.render.destroy(a[2])?;
        }
        3 | 4 => {
            if a[8..].iter().any(|&n| n != 0) {
                return Err(22);
            }
            if a[1] == 3 {
                if a[3] != 0 {
                    return Err(22);
                }
                window.render.batch(rt, a[2], a[5], a[4], a[6], a[7])?;
            } else {
                if a[6] != 0 || a[7] != 0 {
                    return Err(22);
                }
                window.render.raster(rt, a[2], a[5], a[4], a[3])?;
            }
        }
        5 | 6 => {
            window.render.transfer(rt, a, a[1] == 5)?;
        }
        7 => {
            if a[9..].iter().any(|&n| n != 0) {
                return Err(22);
            }
            let target = window.render.target(a[2])?;
            let rect = target.rect(a[3], a[4], a[7], a[8])?;
            let dst = Rect {
                x: a[5] as usize,
                y: a[6] as usize,
                width: rect.width,
                height: rect.height,
            };
            let (width, height) = (
                window.raster_width() as usize,
                window.raster_height() as usize,
            );
            if !dst.valid(width, height) {
                return Err(22);
            }
            let Pixels::Colour(pixels) = &target.pixels else {
                return Err(22);
            };
            if let Some(colour) = window.colour.as_mut() {
                for y in 0..rect.height {
                    let source = (rect.y + y) * target.width + rect.x;
                    let dest = (dst.y + y) * width + dst.x;
                    colour.back[dest..dest + rect.width]
                        .copy_from_slice(&pixels[source..source + rect.width]);
                }
            } else {
                for y in 0..rect.height {
                    let source = (rect.y + y) * target.width + rect.x;
                    let dest = (dst.y + y) * width + dst.x;
                    for (out, &pixel) in window.back[dest..dest + rect.width]
                        .iter_mut()
                        .zip(&pixels[source..source + rect.width])
                    {
                        *out = colour::rgb565(core::rgb(pixel));
                    }
                }
            }
        }
        _ => return Err(22),
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::address_space::{PAGE_CELLS, PRIVATE_START};
    fn setup(format: u32) -> (Runtime, u32) {
        let mut rt = Runtime::new();
        rt.memory.write(BANK, 0);
        let mut request = [8, 8, 0x10000, 2, format, 0, 0, 0, 0, 0, 0, 0];
        rt.graphics_call(1, &mut request).unwrap();
        (rt, request[0])
    }
    fn create(rt: &mut Runtime, window: u32, width: u32, height: u32, format: u32) -> u32 {
        let mut a = [window, 1, 0, width, height, format, 0, 0, 0, 0, 0, 0];
        rt.graphics_call(30, &mut a).unwrap();
        a[2]
    }
    fn put_words(rt: &mut Runtime, at: usize, words: &[u32]) -> u32 {
        let cells: Vec<_> = words
            .iter()
            .flat_map(|&n| [n as u16, (n >> 16) as u16])
            .collect();
        rt.memory.write_from(at, &cells);
        crate::memory::pointer(at) as u32
    }
    fn pixels(rt: &Runtime, window: u32, target: u32) -> Vec<u32> {
        let s = rt.graphics.shared.lock().unwrap();
        let Pixels::Colour(p) = &s.windows[&window].render.targets[&target].pixels else {
            panic!()
        };
        p.clone()
    }
    fn fill(x: u32, y: u32, w: u32, h: u32, colour: u32, opacity: u32) -> [u32; 12] {
        [1, x, y, w, h, colour, opacity, 0, 0, 0, 0, 0]
    }
    fn batch(
        rt: &mut Runtime,
        w: u32,
        t: u32,
        commands: &[[u32; 12]],
        data: &[u16],
    ) -> Result<u32> {
        let commands: Vec<_> = commands.iter().flatten().copied().collect();
        let c = put_words(rt, 3 * BANK - 3, &commands);
        let d = crate::memory::pointer(5 * BANK - 2) as u32;
        rt.memory.write_from(5 * BANK - 2, data);
        rt.graphics_call(
            30,
            &mut [
                w,
                3,
                t,
                0,
                (commands.len() / 12) as u32,
                c,
                if data.is_empty() { 0 } else { d },
                data.len() as u32,
                0,
                0,
                0,
                0,
            ],
        )
    }
    #[test]
    fn probe_limits_and_lifecycle_reject_stale_handles_without_aliasing() {
        let (mut rt, w) = setup(2);
        assert_eq!(rt.graphics_call(30, &mut [0; 12]), Ok(0));
        assert_eq!(
            rt.graphics_call(30, &mut [w, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
            Err(22)
        );
        let t = create(&mut rt, w, 1, 1, A8);
        assert_eq!(
            rt.graphics_call(30, &mut [w, 2, t, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
            Ok(0)
        );
        let next = create(&mut rt, w, 1, 1, A8);
        assert!(next > t);
        assert_eq!(
            rt.graphics_call(30, &mut [w, 2, t, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
            Err(22)
        );
        for _ in 1..MAX_TARGETS {
            create(&mut rt, w, 1, 1, A8);
        }
        assert_eq!(
            rt.graphics_call(30, &mut [w, 1, 0, 1, 1, A8, 0, 0, 0, 0, 0, 0]),
            Err(12)
        );
        let mut state = State::default();
        state
            .create(MAX_WIDTH * 2, MAX_HEIGHT * 2, XRGB8888)
            .unwrap();
        state
            .create(MAX_WIDTH * 2, MAX_HEIGHT * 2, XRGB8888)
            .unwrap();
        assert_eq!(
            state.create(MAX_WIDTH * 2, MAX_HEIGHT * 2, XRGB8888),
            Err(12)
        );
        assert_eq!(state.create(0, 1, A8), Err(22));
        assert_eq!(state.create(1, 1, 1), Err(22));
        state.next = u32::MAX;
        assert_eq!(state.create(1, 1, A8), Err(12));
        assert_eq!(
            rt.graphics_call(2, &mut [w, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
            Ok(0)
        );
        assert_eq!(
            rt.graphics_call(30, &mut [w, 2, next, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
            Err(22)
        );
    }
    #[test]
    fn complete_batch_validation_is_atomic_and_self_copy_is_sequential_memmove() {
        let (mut rt, w) = setup(2);
        let t = create(&mut rt, w, 8, 8, XRGB8888);
        let old = pixels(&rt, w, t);
        let good = fill(0, 0, 8, 8, 0x18293a, 255);
        for mut bad in [
            fill(7, 0, 2, 1, 1, 255),
            fill(0, 0, 1, 1, 0x1000000, 255),
            fill(0, 0, 1, 1, 0, 256),
        ] {
            assert_eq!(batch(&mut rt, w, t, &[good, bad], &[]), Err(22));
            assert_eq!(pixels(&rt, w, t), old);
            bad = good;
            bad[11] = 1;
            assert_eq!(batch(&mut rt, w, t, &[good, bad], &[]), Err(22));
            assert_eq!(pixels(&rt, w, t), old);
        }
        let invalid_source = [4, 0, 0, 1, 1, 0, 0, t + 1, 0, 0, 0, 0];
        assert_eq!(batch(&mut rt, w, t, &[good, invalid_source], &[]), Err(22));
        assert_eq!(pixels(&rt, w, t), old);
        let copy = [4, 1, 1, 7, 7, 0, 0, t, 0, 0, 0, 0];
        let corner = fill(0, 0, 2, 2, 0xf03a7b, 255);
        assert_eq!(batch(&mut rt, w, t, &[good, corner, copy], &[]), Ok(0));
        let actual = pixels(&rt, w, t);
        let mut wanted = vec![core::rgba(0x18293a); 64];
        for y in 0..2 {
            for x in 0..2 {
                wanted[y * 8 + x] = core::rgba(0xf03a7b);
            }
        }
        let before = wanted.clone();
        for y in 0..7 {
            for x in 0..7 {
                wanted[(y + 1) * 8 + x + 1] = before[y * 8 + x];
            }
        }
        assert_eq!(actual, wanted);
    }
    #[test]
    fn borrowed_mask_and_retained_mask_blend_identically_with_low_octets() {
        let (mut rt, w) = setup(2);
        let t = create(&mut rt, w, 8, 8, XRGB8888);
        let a8 = create(&mut rt, w, 3, 2, A8);
        let data = [
            0xa300, 0xa301, 0xa37f, 0xffff, 0x12fd, 0x12ff, 0x1202, 0x1280,
        ];
        rt.memory.write_from(7 * BANK - 2, &data);
        let ptr = crate::memory::pointer(7 * BANK - 2) as u32;
        assert_eq!(
            rt.graphics_call(30, &mut [w, 5, a8, 0, 0, 3, 2, 5, ptr, 8, 0, 0]),
            Ok(0)
        );
        let mask_data = [2, 0, 0, 3, 2, 0x9bc251, 127, 0, 5, 0, 0, 0];
        let mask_target = [3, 4, 0, 3, 2, 0x9bc251, 127, a8, 0, 0, 0, 0];
        assert_eq!(
            batch(
                &mut rt,
                w,
                t,
                &[fill(0, 0, 8, 8, 0x312f8c, 255), mask_data, mask_target],
                &data
            ),
            Ok(0)
        );
        let p = pixels(&rt, w, t);
        for y in 0..2 {
            assert_eq!(&p[y * 8..y * 8 + 3], &p[y * 8 + 4..y * 8 + 7]);
        }
        let old = p.clone();
        let mut invalid = mask_data;
        invalid[7] = 1;
        assert_eq!(
            batch(&mut rt, w, t, &[fill(0, 0, 8, 8, 0, 255), invalid], &data),
            Err(22)
        );
        assert_eq!(pixels(&rt, w, t), old);
    }
    #[test]
    fn transfer_padding_guard_cells_and_fork_cow_are_preserved() {
        let (mut rt, w) = setup(2);
        let t = create(&mut rt, w, 3, 2, XRGB8888);
        rt.memory.enable_processes();
        let at = PRIVATE_START + PAGE_CELLS - 3;
        let input: Vec<u16> = (0..30).map(|i| 0xa300 | i).collect();
        rt.memory.write_from(at, &input);
        let parent = rt.memory.clone();
        let shared = parent.read_page(at).unwrap();
        let ptr = crate::memory::pointer(at) as u32;
        assert_eq!(
            rt.graphics_call(30, &mut [w, 5, t, 0, 0, 3, 2, 12, ptr, 30, 0, 0]),
            Ok(0)
        );
        assert_eq!(rt.memory.cells(at..at + 30).collect::<Vec<_>>(), input);
        assert_eq!(
            rt.graphics_call(30, &mut [w, 6, t, 0, 0, 3, 2, 12, ptr, 30, 0, 0]),
            Ok(0)
        );
        for i in 0..30 {
            let valid = i < 9 || (12..21).contains(&i);
            assert_eq!(
                rt.memory.read(at + i),
                if valid { input[i] & 255 } else { input[i] }
            );
        }
        assert_eq!(parent.cells(at..at + 30).collect::<Vec<_>>(), input);
        assert!(Arc::ptr_eq(&shared, &parent.read_page(at).unwrap()));
        assert!(!Arc::ptr_eq(&shared, &rt.memory.read_page(at).unwrap()));
        let before = pixels(&rt, w, t);
        for (index, value, error) in [(7, 8, 22), (8, 0, 14), (8, 1, 14), (9, 20, 22), (10, 1, 22)]
        {
            let mut bad = [w, 5, t, 0, 0, 3, 2, 12, ptr, 30, 0, 0];
            bad[index] = value;
            assert_eq!(rt.graphics_call(30, &mut bad), Err(error));
            assert_eq!(pixels(&rt, w, t), before);
        }
    }
    #[test]
    fn retained_raster_matches_borrowed_raster_and_late_invalid_is_atomic() {
        let (mut rt, w) = setup(2);
        let t = create(&mut rt, w, 31, 17, A8);
        let primitive_words = [
            i32::MIN as u32,
            0,
            i32::MAX as u32,
            16,
            0,
            2,
            7,
            6,
            0,
            0,
            1,
            3,
        ];
        let list = put_words(&mut rt, 9 * BANK - 2, &primitive_words);
        assert_eq!(
            rt.graphics_call(30, &mut [w, 4, t, 1, 2, list, 0, 0, 0, 0, 0, 0]),
            Ok(0)
        );
        let at = 11 * BANK - 1;
        let ptr = crate::memory::pointer(at) as u32;
        rt.memory.fill(at..at + 31 * 17, 0xdead);
        assert_eq!(
            rt.graphics_call(30, &mut [w, 6, t, 0, 0, 31, 17, 31, ptr, 31 * 17, 0, 0]),
            Ok(0)
        );
        let retained = rt.memory.cells(at..at + 31 * 17).collect::<Vec<_>>();
        let old_list: Vec<_> = primitive_words
            .chunks_exact(6)
            .flat_map(|p| {
                let mut cells: Vec<u16> = p[..4]
                    .iter()
                    .flat_map(|&n| [n as u16, (n >> 16) as u16])
                    .collect();
                cells.extend([p[4] as u16, p[5] as u16]);
                cells
            })
            .collect();
        rt.memory.write_from(13 * BANK - 1, &old_list);
        let old_ptr = crate::memory::pointer(13 * BANK - 1) as u32;
        assert_eq!(
            rt.graphics_call(29, &mut [ptr, old_ptr, 31, 17, 31, 2, 1, 0, 0, 0, 0, 0]),
            Ok(0)
        );
        assert_eq!(
            rt.memory.cells(at..at + 31 * 17).collect::<Vec<_>>(),
            retained
        );
        let mut invalid = primitive_words;
        invalid[11] = 9;
        let list = put_words(&mut rt, 9 * BANK - 2, &invalid);
        assert_eq!(
            rt.graphics_call(30, &mut [w, 4, t, 1, 2, list, 0, 0, 0, 0, 0, 0]),
            Err(22)
        );
        assert_eq!(
            rt.graphics_call(30, &mut [w, 6, t, 0, 0, 31, 17, 31, ptr, 31 * 17, 0, 0]),
            Ok(0)
        );
        assert_eq!(
            rt.memory.cells(at..at + 31 * 17).collect::<Vec<_>>(),
            retained
        );
    }
    #[test]
    fn blit_preserves_colour_and_explicit_rgb565_quantization_without_presenting() {
        for format in [1, 2] {
            let (mut rt, w) = setup(format);
            let t = create(&mut rt, w, 8, 8, XRGB8888);
            batch(&mut rt, w, t, &[fill(0, 0, 8, 8, 0x27b35a, 255)], &[]).unwrap();
            let old_front = rt.graphics.shared.lock().unwrap().windows[&w].front.clone();
            assert_eq!(
                rt.graphics_call(30, &mut [w, 7, t, 0, 0, 1, 2, 4, 3, 0, 0, 0]),
                Ok(0)
            );
            {
                let s = rt.graphics.shared.lock().unwrap();
                let win = &s.windows[&w];
                assert!(Arc::ptr_eq(&win.front, &old_front));
                if format == 1 {
                    assert_eq!(win.back[17], colour::rgb565(0x27b35a));
                    assert_eq!(win.back[0], 0);
                } else {
                    assert_eq!(win.colour.as_ref().unwrap().back[17], core::rgba(0x27b35a));
                    assert_eq!(win.colour.as_ref().unwrap().front[17], core::rgba(0));
                }
            }
            assert_eq!(
                rt.graphics_call(30, &mut [w, 7, t, 0, 0, 7, 0, 4, 3, 0, 0, 0]),
                Err(22)
            );
        }
    }
}
