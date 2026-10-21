//! Opaque full-colour hosted surfaces. Guest C16 cells never become host pointers.
use std::sync::Arc;

pub(super) struct TrueColour {
    pub(super) back: Vec<u32>,
    pub(super) front: Arc<Vec<u32>>,
}

pub(super) fn rgba(rgb: u32) -> u32 {
    u32::from_le_bytes([(rgb >> 16) as u8, (rgb >> 8) as u8, rgb as u8, 255])
}

pub(super) fn rgba565(pixel: u16) -> u32 {
    let [r, g, b] = rvm::display_rgb565::rgb565_to_rgb888(pixel);
    u32::from_le_bytes([r, g, b, 255])
}

pub(super) fn rgb565(rgb: u32) -> u16 {
    (((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f)) as u16
}

pub(super) fn resized<T: Copy>(
    source: &[T],
    width: u32,
    height: u32,
    new_width: u32,
    new_height: u32,
    black: T,
) -> Vec<T> {
    let mut pixels = vec![black; (new_width * new_height) as usize];
    for row in 0..height.min(new_height) {
        let count = width.min(new_width) as usize;
        pixels[(row * new_width) as usize..(row * new_width) as usize + count]
            .copy_from_slice(&source[(row * width) as usize..(row * width) as usize + count]);
    }
    pixels
}

pub(super) fn copy_rect<T: Copy>(
    pixels: &mut [T],
    stride: u32,
    x: u32,
    y: u32,
    w: u32,
    h: u32,
    dx: u32,
    dy: u32,
) {
    // Snapshot before writing: both axes may overlap.
    let mut copy = Vec::with_capacity((w * h) as usize);
    for row in 0..h {
        let at = ((y + row) * stride + x) as usize;
        copy.extend_from_slice(&pixels[at..at + w as usize]);
    }
    for row in 0..h {
        let at = ((dy + row) * stride + dx) as usize;
        pixels[at..at + w as usize]
            .copy_from_slice(&copy[(row * w) as usize..((row + 1) * w) as usize]);
    }
}

impl TrueColour {
    pub(super) fn new(count: usize) -> Self {
        let back = vec![rgba(0); count];
        Self {
            front: Arc::new(back.clone()),
            back,
        }
    }

    pub(super) fn present(&mut self) {
        self.front = Arc::new(self.back.clone());
    }
}
