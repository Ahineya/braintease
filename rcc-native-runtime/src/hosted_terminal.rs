//! CoreText glyph masks and iTerm profile colours. No AppKit views, guest
//! pointers or graphics contexts are shared across threads. The worker owns
//! its font/cache; the main thread receives immutable Retina RGBA frames.
#![allow(unexpected_cfgs)]
use objc::{class, msg_send, runtime::Object, sel, sel_impl};
use std::{
    cell::RefCell,
    collections::HashMap,
    ffi::{c_void, CString},
};

type CF = *const c_void;
#[repr(C)]
#[derive(Clone, Copy)]
struct Size {
    width: f64,
    height: f64,
}
#[link(name = "CoreText", kind = "framework")]
unsafe extern "C" {
    fn CTFontCreateWithName(name: CF, size: f64, matrix: CF) -> CF;
    fn CTFontGetAscent(font: CF) -> f64;
    fn CTFontGetDescent(font: CF) -> f64;
    fn CTFontGetLeading(font: CF) -> f64;
    fn CTFontCopyPostScriptName(font: CF) -> CF;
    fn CTFontGetGlyphsForCharacters(
        font: CF,
        chars: *const u16,
        glyphs: *mut u16,
        count: isize,
    ) -> bool;
    fn CTFontGetAdvancesForGlyphs(
        font: CF,
        orientation: u32,
        glyphs: *const u16,
        advances: *mut Size,
        count: isize,
    ) -> f64;
    fn CTLineCreateWithAttributedString(text: CF) -> CF;
    fn CTLineDraw(line: CF, context: CF);
    static kCTFontAttributeName: CF;
    static kCTForegroundColorFromContextAttributeName: CF;
    static kCTLigatureAttributeName: CF;
}
#[link(name = "CoreGraphics", kind = "framework")]
unsafe extern "C" {
    fn CGColorSpaceCreateDeviceRGB() -> CF;
    fn CGColorSpaceRelease(space: CF);
    fn CGBitmapContextCreate(
        data: *mut c_void,
        width: usize,
        height: usize,
        bits: usize,
        stride: usize,
        space: CF,
        info: u32,
    ) -> CF;
    fn CGContextRelease(context: CF);
    fn CGContextSetGrayFillColor(context: CF, gray: f64, alpha: f64);
    fn CGContextSetTextPosition(context: CF, x: f64, y: f64);
    fn CGContextSetShouldAntialias(context: CF, value: bool);
    fn CGContextSetShouldSmoothFonts(context: CF, value: bool);
}
#[link(name = "CoreFoundation", kind = "framework")]
unsafe extern "C" {
    fn CFRelease(value: CF);
}

unsafe fn string(text: &str) -> *mut Object {
    let text = CString::new(text).unwrap();
    unsafe { msg_send![class!(NSString),stringWithUTF8String:text.as_ptr()] }
}
unsafe fn text(value: *mut Object) -> Option<String> {
    if value.is_null() {
        return None;
    }
    let p: *const std::ffi::c_char = unsafe { msg_send![value, UTF8String] };
    if p.is_null() {
        None
    } else {
        Some(
            unsafe { std::ffi::CStr::from_ptr(p) }
                .to_string_lossy()
                .into_owned(),
        )
    }
}
unsafe fn get(object: *mut Object, key: &str) -> *mut Object {
    if object.is_null() {
        return std::ptr::null_mut();
    }
    unsafe { msg_send![object,objectForKey:string(key)] }
}

struct Renderer {
    font: CF,
    width: u32,
    height: u32,
    baseline: f64,
    palette: [[u8; 3]; 16],
    masks: HashMap<(String, u16), Vec<u8>>,
}
impl Drop for Renderer {
    fn drop(&mut self) {
        unsafe { CFRelease(self.font) }
    }
}
thread_local! {static RENDERER:RefCell<Renderer>=RefCell::new(Renderer::new());}

impl Renderer {
    fn new() -> Self {
        objc::rc::autoreleasepool(|| unsafe {
            let defaults: *mut Object = msg_send![class!(NSUserDefaults), alloc];
            let defaults: *mut Object =
                msg_send![defaults,initWithSuiteName:string("com.googlecode.iterm2")];
            let guid = get(defaults, "Default Bookmark Guid");
            let profiles = get(defaults, "New Bookmarks");
            let count: usize = if profiles.is_null() {
                0
            } else {
                msg_send![profiles, count]
            };
            let mut profile: *mut Object = std::ptr::null_mut();
            for i in 0..count {
                let item: *mut Object = msg_send![profiles,objectAtIndex:i];
                let item_guid = get(item, "Guid");
                let equal: bool = if guid.is_null() || item_guid.is_null() {
                    false
                } else {
                    msg_send![item_guid,isEqualToString:guid]
                };
                if equal || profile.is_null() {
                    profile = item;
                }
                if equal {
                    break;
                }
            }
            let spec = std::env::var("FUSE_X_TERM_FONT")
                .ok()
                .or_else(|| text(get(profile, "Normal Font")))
                .unwrap_or_else(|| "Menlo 13".into());
            let (name, size) = spec
                .rsplit_once(' ')
                .and_then(|(n, s)| s.parse::<f64>().ok().map(|s| (n, s)))
                .filter(|(_, s)| s.is_finite() && (6.0..=32.0).contains(s))
                .unwrap_or(("Menlo", 13.0));
            let font = CTFontCreateWithName(string(name).cast(), size * 2.0, std::ptr::null());
            if std::env::var_os("FUSE_X_TERM_TRACE").is_some() {
                let resolved = CTFontCopyPostScriptName(font);
                eprintln!(
                    "Fuse-X terminal font: requested {name} {size}, resolved {}",
                    text(resolved as *mut Object).unwrap_or_default()
                );
                CFRelease(resolved);
            }
            let mut glyph = 0;
            CTFontGetGlyphsForCharacters(font, &('M' as u16), &mut glyph, 1);
            let mut advance = Size {
                width: 0.0,
                height: 0.0,
            };
            CTFontGetAdvancesForGlyphs(font, 0, &glyph, &mut advance, 1);
            let ascent = CTFontGetAscent(font);
            let descent = CTFontGetDescent(font);
            let width = (advance.width / 2.0).ceil().max(1.0) as u32;
            let height = ((ascent + descent + CTFontGetLeading(font)) / 2.0)
                .ceil()
                .max(1.0) as u32;
            let baseline = descent + ((height * 2) as f64 - ascent - descent) / 2.0;
            let system: *mut Object = msg_send![class!(NSUserDefaults), standardUserDefaults];
            let dark = std::env::var("FUSE_X_TERM_APPEARANCE")
                .ok()
                .unwrap_or_else(|| {
                    text(get(system, "AppleInterfaceStyle")).unwrap_or_else(|| "dark".into())
                })
                .eq_ignore_ascii_case("dark");
            let separate = get(profile, "Use Separate Colors for Light and Dark Mode");
            let separate: bool = if separate.is_null() {
                false
            } else {
                msg_send![separate, boolValue]
            };
            let suffix = if separate {
                if dark {
                    " (Dark)"
                } else {
                    " (Light)"
                }
            } else {
                ""
            };
            let mut palette = [[0; 3]; 16];
            let fallback = [
                [20, 25, 31],
                [180, 60, 42],
                [70, 170, 100],
                [195, 170, 65],
                [65, 90, 180],
                [160, 80, 170],
                [60, 160, 175],
                [215, 218, 223],
                [100, 105, 115],
                [225, 120, 115],
                [115, 210, 145],
                [225, 205, 110],
                [145, 165, 235],
                [210, 145, 215],
                [130, 220, 225],
                [245, 245, 245],
            ];
            for i in 0..16 {
                let key = if i == 0 {
                    "Background Color".into()
                } else if i == 7 {
                    "Foreground Color".into()
                } else {
                    format!("Ansi {i} Color")
                };
                let mut colour = get(profile, &format!("{key}{suffix}"));
                if colour.is_null() {
                    colour = get(profile, &key);
                }
                for (j, component) in ["Red Component", "Green Component", "Blue Component"]
                    .iter()
                    .enumerate()
                {
                    let value = get(colour, component);
                    palette[i][j] = if value.is_null() {
                        fallback[i][j]
                    } else {
                        let value: f64 = msg_send![value, doubleValue];
                        (value.clamp(0.0, 1.0) * 255.0).round() as u8
                    };
                }
            }
            let _: () = msg_send![defaults, release];
            Self {
                font,
                width,
                height,
                baseline,
                palette,
                masks: HashMap::new(),
            }
        })
    }
    fn mask(&mut self, text: &str, width: u16) -> &[u8] {
        let key = (text.to_owned(), width);
        if !self.masks.contains_key(&key) {
            // Bounded per-worker cache; normal ASCII/screens remain resident.
            if self.masks.len() >= 4096 {
                self.masks.clear();
            }
            let (w, h) = (
                self.width as usize * 2 * width as usize,
                self.height as usize * 2,
            );
            let mut rgba = vec![0u8; w * h * 4];
            objc::rc::autoreleasepool(|| unsafe {
                let space = CGColorSpaceCreateDeviceRGB();
                let context =
                    CGBitmapContextCreate(rgba.as_mut_ptr().cast(), w, h, 8, w * 4, space, 1);
                assert!(!context.is_null());
                CGContextSetShouldAntialias(context, true);
                CGContextSetShouldSmoothFonts(context, false);
                CGContextSetGrayFillColor(context, 1.0, 1.0);
                CGContextSetTextPosition(context, 0.0, self.baseline);
                let attributes: *mut Object = msg_send![class!(NSMutableDictionary), dictionary];
                let _: () = msg_send![attributes,setObject:self.font forKey:kCTFontAttributeName];
                let yes: *mut Object = msg_send![class!(NSNumber),numberWithBool:true];
                let zero: *mut Object = msg_send![class!(NSNumber),numberWithInt:0i32];
                let _: () = msg_send![attributes,setObject:yes forKey:kCTForegroundColorFromContextAttributeName];
                let _: () = msg_send![attributes,setObject:zero forKey:kCTLigatureAttributeName];
                let attributed: *mut Object = msg_send![class!(NSAttributedString), alloc];
                let attributed: *mut Object =
                    msg_send![attributed,initWithString:string(text) attributes:attributes];
                let line = CTLineCreateWithAttributedString(attributed.cast());
                CTLineDraw(line, context);
                CFRelease(line);
                let _: () = msg_send![attributed, release];
                CGContextRelease(context);
                CGColorSpaceRelease(space);
            });
            self.masks
                .insert(key.clone(), rgba.chunks_exact(4).map(|p| p[3]).collect());
        }
        &self.masks[&key]
    }
}
pub(super) fn metrics() -> (u32, u32) {
    RENDERER.with(|r| {
        let r = r.borrow();
        (r.width, r.height)
    })
}
#[derive(Clone, Debug)]
pub(super) struct Cell {
    pub text: String,
    pub width: u16,
    pub colour: u16,
}
pub(super) struct Frame {
    pub back: Vec<u32>,
    pub front: std::sync::Arc<Vec<u32>>,
    pub width: u32,
    pub height: u32,
}
impl Frame {
    pub fn from_surface(width: u32, height: u32, back: &[u16], front: &[u16]) -> Self {
        fn expand(source: &[u16], width: usize) -> Vec<u32> {
            let mut out = Vec::with_capacity(source.len() * 4);
            for row in source.chunks_exact(width) {
                let start = out.len();
                for p in row {
                    let [r, g, b] = rvm::display_rgb565::rgb565_to_rgb888(*p);
                    let p = u32::from_le_bytes([r, g, b, 255]);
                    out.extend([p, p]);
                }
                out.extend_from_within(start..);
            }
            out
        }
        Self {
            width,
            height,
            back: expand(back, width as usize),
            front: std::sync::Arc::new(expand(front, width as usize)),
        }
    }
    #[cfg(test)]
    pub fn new(width: u32, height: u32) -> Self {
        let back = vec![0xff000000; (width * height * 4) as usize];
        Self {
            front: std::sync::Arc::new(back.clone()),
            back,
            width,
            height,
        }
    }
    pub fn row(&mut self, row: u32, cells: &[Cell], cursor: Option<(u32, u32, u32)>) {
        RENDERER.with(|r| {
            let mut r = r.borrow_mut();
            let (cw, ch) = (r.width as usize * 2, r.height as usize * 2);
            debug_assert!((row as usize + 1) * ch <= self.height as usize * 2);
            let stride = self.width as usize * 2;
            let top = row as usize * ch;
            // Paint backgrounds first: a wide glyph crosses the continuation.
            for (x, cell) in cells.iter().enumerate() {
                let bg = r.palette[((cell.colour >> 4) & 15) as usize];
                let colour = u32::from_le_bytes([bg[0], bg[1], bg[2], 255]);
                for y in 0..ch {
                    self.back[(top + y) * stride + x * cw..(top + y) * stride + (x + 1) * cw]
                        .fill(colour);
                }
            }
            for (x, cell) in cells.iter().enumerate() {
                if cell.width == 0 {
                    continue;
                }
                let fg = r.palette[(cell.colour & 15) as usize];
                let bg = r.palette[((cell.colour >> 4) & 15) as usize];
                let mask = if cell.text == " " {
                    None
                } else {
                    Some(r.mask(&cell.text, cell.width))
                };
                let width = cw * cell.width as usize;
                for y in 0..ch {
                    for dx in 0..width {
                        let alpha = if cell.colour & 256 != 0 && y >= ch - 3 && y < ch - 1 {
                            255
                        } else {
                            mask.map_or(0, |m| m[y * width + dx])
                        } as u32;
                        if alpha == 0 {
                            continue;
                        }
                        let blend = |i: usize| {
                            ((fg[i] as u32 * alpha + bg[i] as u32 * (255 - alpha) + 127) / 255)
                                as u8
                        };
                        self.back[(top + y) * stride + x * cw + dx] =
                            u32::from_le_bytes([blend(0), blend(1), blend(2), 255]);
                    }
                }
            }
            if let Some((x, y, shape)) = cursor.filter(|(_, y, _)| *y == row) {
                let _ = y;
                let fg = r.palette[(cells[x as usize].colour & 15) as usize];
                let colour = u32::from_le_bytes([fg[0], fg[1], fg[2], 255]);
                for dy in 0..ch {
                    for dx in 0..cw {
                        if (shape == 3 || shape == 4) && dy < ch - 3 {
                            continue;
                        }
                        if (shape == 5 || shape == 6) && dx >= 2 {
                            continue;
                        }
                        if shape <= 2 {
                            self.back[(top + dy) * stride + x as usize * cw + dx] ^= 0x00ffffff;
                        } else {
                            self.back[(top + dy) * stride + x as usize * cw + dx] = colour;
                        }
                    }
                }
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn font_preview_and_cached_full_frame() {
        let (cw, ch) = metrics();
        let cols = 160;
        let rows = 55;
        let mut frame = Frame::new(cols * cw, rows * ch);
        let lines = [
            "Fuse-X / Native terminal / MesloLGS NF",
            "The quick brown fox jumps over the lazy dog. 0123456789",
            "Pavlo: Ukrainian text / Привіт, світе!",
            "Window | Rift | Music | mux tabs and smooth resizing",
        ];
        for row in 0..rows {
            let text = lines[row as usize % lines.len()];
            let mut cells: Vec<_> = text
                .chars()
                .map(|c| Cell {
                    text: c.to_string(),
                    width: 1,
                    colour: 7,
                })
                .collect();
            cells.resize(
                cols as usize,
                Cell {
                    text: " ".into(),
                    width: 1,
                    colour: 7,
                },
            );
            if row >= 4 {
                for (i, c) in cells.iter_mut().enumerate() {
                    c.colour = ((row % 8) as u16) << 4 | (i % 16) as u16;
                }
            }
            frame.row(row, &cells, None);
        }
        if let Some(path) = std::env::var_os("FUSE_X_TERM_TEST_PREVIEW") {
            let mut bytes =
                format!("P6\n{} {}\n255\n", frame.width * 2, frame.height * 2).into_bytes();
            for p in &frame.back {
                bytes.extend_from_slice(&p.to_le_bytes()[..3]);
            }
            std::fs::write(path, bytes).unwrap();
        }
        let cells = vec![
            Cell {
                text: "A".into(),
                width: 1,
                colour: 7
            };
            cols as usize
        ];
        let now = std::time::Instant::now();
        for _ in 0..10 {
            for row in 0..rows {
                frame.row(row, &cells, None);
            }
        }
        eprintln!(
            "native cached {}x{} frame ({}x{} logical pixels): {:?} average",
            cols,
            rows,
            frame.width,
            frame.height,
            now.elapsed() / 10
        );
    }
}
