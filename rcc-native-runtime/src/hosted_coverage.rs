//! Generic clipped A8 coverage in borrowed caller RAM. Geometry is copied and
//! validated before any destination write; native scratch storage never escapes.
use super::*;

const PRIMITIVE_CELLS: usize = 10;
const PRIMITIVE_LIMIT: usize = 8192;
use super::render::core::{raster_a8, Primitive};

fn signed(cells: &[u16]) -> i32 {
    (u32::from(cells[0]) | (u32::from(cells[1]) << 16)) as i32
}

pub(super) fn raster(rt: &mut Runtime, a: &[u32; 12]) -> Result<u32> {
    if a.iter().all(|&value| value == 0) { return Ok(0); }
    let (width, height, stride, count, flags) = (a[2], a[3], a[4], a[5], a[6]);
    if width == 0 || height == 0 || width > MAX_WIDTH * 2 || height > MAX_HEIGHT * 2
        || stride < width || count as usize > PRIMITIVE_LIMIT || flags & !1 != 0
        || a[7..].iter().any(|&value| value != 0) {
        return Err(22);
    }
    let cells = (height - 1).checked_mul(stride).and_then(|n| n.checked_add(width)).ok_or(14u16)? as usize;
    let dst = span(a[0], cells)?;
    if dst < BANK { return Err(14); }
    let list_cells = count as usize * PRIMITIVE_CELLS;
    let list = if list_cells != 0 || a[1] != 0 { Some(span(a[1], list_cells)?) } else { None };
    if let Some(list) = list {
        if list_cells != 0 && dst < list + list_cells && list < dst + cells { return Err(22); }
    }
    let mut packed = vec![0; list_cells];
    if let Some(list) = list { rt.memory.read_into(list, &mut packed); }
    let mut primitives = Vec::with_capacity(count as usize);
    for p in packed.chunks_exact(PRIMITIVE_CELLS) {
        if p[8] > 1 || p[9] > 8 || (p[8] == 0 && p[9] == 0) { return Err(22); }
        primitives.push(Primitive { x0: signed(p), y0: signed(&p[2..]), x1: signed(&p[4..]),
                                    y1: signed(&p[6..]), dot: p[8] == 1, size: p[9] });
    }
    drop(packed);
    let (width, height, stride) = (width as usize, height as usize, stride as usize);
    let mut coverage = vec![0; width * height];
    let mut row = vec![0; width];
    if flags & 1 == 0 {
        for y in 0..height {
            rt.memory.read_into(dst + y * stride, &mut row);
            for (out, &value) in coverage[y * width..(y + 1) * width].iter_mut().zip(&row) {
                *out = (value & 255) as u8;
            }
        }
    }
    raster_a8(&mut coverage, width, height, width, &primitives, false).map_err(|_|22u16)?;
    for y in 0..height {
        for (out, &value) in row.iter_mut().zip(&coverage[y * width..(y + 1) * width]) {
            *out = u16::from(value);
        }
        rt.memory.write_from(dst + y * stride, &row);
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::address_space::{PAGE_CELLS, PRIVATE_START};

    fn packed(p: Primitive) -> Vec<u16> {
        let mut cells = Vec::new();
        for value in [p.x0, p.y0, p.x1, p.y1] {
            cells.extend([value as u16, ((value as u32) >> 16) as u16]);
        }
        cells.extend([u16::from(p.dot), p.size]);
        cells
    }
    fn request(dst: usize, list: usize, width: u32, height: u32, stride: u32, count: u32, flags: u32) -> [u32; 12] {
        [crate::memory::pointer(dst) as u32,
         if list == 0 { 0 } else { crate::memory::pointer(list) as u32 },
         width, height, stride, count, flags, 0, 0, 0, 0, 0]
    }
    fn primitive(x0: i32, y0: i32, x1: i32, y1: i32, dot: bool, size: u16) -> Primitive {
        Primitive { x0, y0, x1, y1, dot, size }
    }

    #[test]
    fn probe_and_validation_are_atomic_including_the_final_primitive() {
        let mut rt = Runtime::new();
        assert_eq!(raster(&mut rt, &[0; 12]), Ok(0));
        let (dst, list) = (3 * BANK - 5, 5 * BANK - 2);
        let initial = vec![0x573; 137];
        rt.memory.write_from(dst, &initial);
        let mut p = packed(primitive(0, 0, 7, 3, false, 1));
        p.extend(packed(primitive(6, 5, 0, 0, true, 2)));
        rt.memory.write_from(list, &p);
        let a = request(dst, list, 8, 8, 17, 2, 1);
        for (index, value, error) in [(0, 0, 14), (1, 0, 14), (1, a[0], 22),
            (2, 0, 22), (2, MAX_WIDTH * 2 + 1, 22), (3, MAX_HEIGHT * 2 + 1, 22),
            (4, 7, 22), (4, u32::MAX, 14), (5, 8193, 22), (6, 2, 22), (7, 1, 22)] {
            let mut bad = a;
            bad[index] = value;
            assert_eq!(raster(&mut rt, &bad), Err(error), "argument {index}");
            assert_eq!(rt.memory.cells(dst..dst + initial.len()).collect::<Vec<_>>(), initial);
        }
        for (field, value) in [(18, 2), (19, 9), (18, 0)] {
            let mut bad = p.clone();
            bad[field] = value;
            if field == 18 && value == 0 { bad[19] = 0; }
            rt.memory.write_from(list, &bad);
            assert_eq!(raster(&mut rt, &a), Err(22));
            assert_eq!(rt.memory.cells(dst..dst + initial.len()).collect::<Vec<_>>(), initial);
        }
    }

    #[test]
    fn clipping_octants_dots_and_max_coverage_preserve_padding_and_reversal() {
        let mut rt = Runtime::new();
        let (dst, list) = (3 * BANK - 2, 8 * BANK - 1);
        let (w, h, stride) = (73usize, 59usize, 81usize);
        let initial = vec![0x3d7; stride * h];
        for size in 1..=8 {
            let cases = [primitive(-10000, -4000, 10000, 4000, false, size),
                primitive(i32::MIN, i32::MIN, i32::MAX, i32::MAX, false, size),
                primitive(i32::MIN, 28, i32::MAX, 28, false, size),
                primitive(35, i32::MIN, 35, i32::MAX, false, size),
                primitive(35, 28, 65, 40, false, size), primitive(35, 28, 50, 54, false, size),
                primitive(35, 28, 20, 54, false, size), primitive(35, 28, 4, 40, false, size),
                primitive(35, 28, 4, 8, false, size), primitive(35, 28, 20, 3, false, size),
                primitive(35, 28, 50, 3, false, size), primitive(35, 28, 65, 8, false, size)];
            for p in cases {
                rt.memory.write_from(dst, &initial);
                rt.memory.write_from(list, &packed(p));
                let mut a = request(dst, list, w as u32, h as u32, stride as u32, 1, 1);
                raster(&mut rt, &a).unwrap();
                let forward: Vec<_> = rt.memory.cells(dst..dst + initial.len()).collect();
                rt.memory.write_from(list, &packed(Primitive { x0: p.x1, y0: p.y1, x1: p.x0, y1: p.y0, ..p }));
                raster(&mut rt, &a).unwrap();
                let reverse: Vec<_> = rt.memory.cells(dst..dst + initial.len()).collect();
                assert!(forward.iter().zip(&reverse).all(|(&a, &b)| a.abs_diff(b) <= 1));
                for y in 0..h { assert!(forward[y * stride + w..(y + 1) * stride].iter().all(|&v| v == 0x3d7)); }
                // Overlay of the same primitive does not add opacity.
                a[6] = 0;
                raster(&mut rt, &a).unwrap();
                assert_eq!(rt.memory.cells(dst..dst + initial.len()).collect::<Vec<_>>(), reverse);
            }
            rt.memory.write_from(list, &packed(primitive(35, 28, 0, 0, true, size)));
            let a = request(dst, list, w as u32, h as u32, stride as u32, 1, 1);
            raster(&mut rt, &a).unwrap();
            for y in 0..h { for x in 0..w {
                let value = rt.memory.read(dst + y * stride + x);
                if (x as i32 - 35).abs() <= i32::from(size) && (y as i32 - 28).abs() <= i32::from(size) {
                    assert_eq!(value, rt.memory.read(dst + (56 - y) * stride + 70 - x));
                } else { assert_eq!(value, 0); }
            }}
        }
    }

    #[test]
    fn clear_overlay_low_octets_cross_pages_and_forked_snapshots() {
        let mut rt = Runtime::new();
        rt.memory.enable_processes();
        let (dst, list) = (PRIVATE_START + PAGE_CELLS - 2, 5 * BANK - 3);
        let (w, h, stride) = (17usize, 6usize, 23usize);
        let initial: Vec<_> = (0..h * stride).map(|i| 0xa300 | ((i * 13) & 255) as u16).collect();
        rt.memory.write_from(dst, &initial);
        rt.memory.write_from(list, &packed(primitive(-2, 0, 18, 5, false, 2)));
        let parent = rt.memory.clone();
        let snapshot = parent.read_page(dst + 3).unwrap();
        let a = request(dst, list, w as u32, h as u32, stride as u32, 1, 0);
        raster(&mut rt, &a).unwrap();
        assert_eq!(parent.cells(dst..dst + initial.len()).collect::<Vec<_>>(), initial);
        assert!(Arc::ptr_eq(&snapshot, &parent.read_page(dst + 3).unwrap()));
        assert!(!Arc::ptr_eq(&snapshot, &rt.memory.read_page(dst + 3).unwrap()));
        for y in 0..h { for x in 0..stride {
            let value = rt.memory.read(dst + y * stride + x);
            if x < w { assert!(value <= 255 && value >= initial[y * stride + x] & 255); }
            else { assert_eq!(value, initial[y * stride + x]); }
        }}
        let a = request(dst, 0, w as u32, h as u32, stride as u32, 0, 1);
        raster(&mut rt, &a).unwrap();
        for y in 0..h { for x in 0..stride {
            assert_eq!(rt.memory.read(dst + y * stride + x), if x < w { 0 } else { initial[y * stride + x] });
        }}
    }
}
