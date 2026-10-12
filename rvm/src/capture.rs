//! Native-resolution capture of completed display frames, with exact RGB565 comparison.
use crate::display_rgb565::{rgb565_to_rgb888, FrameSnapshot};
use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::path::Path;

fn validate_frame(frame: &FrameSnapshot) -> Result<(), String> {
    if frame.width == 0
        || frame.height == 0
        || frame.pixels.len() != frame.width as usize * frame.height as usize
    {
        return Err("Cannot capture a frame with invalid dimensions or pixel count".to_string());
    }
    Ok(())
}

fn create_parent(path: &Path) -> Result<(), String> {
    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        fs::create_dir_all(parent)
            .map_err(|error| format!("Cannot create {}: {error}", parent.display()))?;
    }
    Ok(())
}

/// Encode RGB888 PNG using the same conversion as the interactive window.
pub fn write_png(frame: &FrameSnapshot, path: &Path) -> Result<(), String> {
    validate_frame(frame)?;
    create_parent(path)?;
    let file =
        File::create(path).map_err(|error| format!("Cannot create {}: {error}", path.display()))?;
    let mut output = BufWriter::new(file);
    {
        let mut encoder = png::Encoder::new(&mut output, frame.width as u32, frame.height as u32);
        encoder.set_color(png::ColorType::Rgb);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder
            .write_header()
            .map_err(|error| format!("Cannot encode {}: {error}", path.display()))?;
        let mut rgb = Vec::with_capacity(frame.pixels.len() * 3);
        for color in &frame.pixels {
            rgb.extend_from_slice(&rgb565_to_rgb888(*color));
        }
        writer
            .write_image_data(&rgb)
            .map_err(|error| format!("Cannot write {}: {error}", path.display()))?;
        writer
            .finish()
            .map_err(|error| format!("Cannot finish {}: {error}", path.display()))?;
    }
    output
        .flush()
        .map_err(|error| format!("Cannot flush {}: {error}", path.display()))
}

/// Raw captures contain two host octets per pixel, least significant octet first.
pub fn write_raw(frame: &FrameSnapshot, path: &Path) -> Result<(), String> {
    validate_frame(frame)?;
    create_parent(path)?;
    let file =
        File::create(path).map_err(|error| format!("Cannot create {}: {error}", path.display()))?;
    let mut writer = BufWriter::new(file);
    for color in &frame.pixels {
        writer
            .write_all(&color.to_le_bytes())
            .map_err(|error| format!("Cannot write {}: {error}", path.display()))?;
    }
    writer
        .flush()
        .map_err(|error| format!("Cannot flush {}: {error}", path.display()))
}

pub fn capture_frame(
    frame: &FrameSnapshot,
    png_path: Option<&Path>,
    raw_path: Option<&Path>,
) -> Result<(), String> {
    validate_frame(frame)?;
    if let Some(path) = png_path {
        write_png(frame, path)?;
    }
    if let Some(path) = raw_path {
        write_raw(frame, path)?;
    }
    Ok(())
}

/// Compare exact pixels. A mismatch writes expected, actual, and red-on-black diff PNGs.
/// Expected files use raw RGB565 little-endian data and the captured frame's dimensions.
pub fn compare_raw(
    frame: &FrameSnapshot,
    expected: &Path,
    artifacts: &Path,
) -> Result<bool, String> {
    validate_frame(frame)?;
    let bytes = fs::read(expected)
        .map_err(|error| format!("Cannot read {}: {error}", expected.display()))?;
    if bytes.len() != frame.pixels.len() * 2 {
        return Err(format!(
            "Expected frame {} has {} octets; {}x{} requires {}",
            expected.display(),
            bytes.len(),
            frame.width,
            frame.height,
            frame.pixels.len() * 2
        ));
    }
    let expected_pixels: Vec<u16> = bytes
        .chunks_exact(2)
        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
        .collect();
    if expected_pixels == frame.pixels {
        return Ok(true);
    }
    let mut expected_frame = frame.clone();
    expected_frame.pixels = expected_pixels;
    let mut diff = frame.clone();
    diff.pixels = frame
        .pixels
        .iter()
        .zip(&expected_frame.pixels)
        .map(|(actual, expected)| if actual == expected { 0 } else { 0xf800 })
        .collect();
    write_png(&expected_frame, &artifacts.join("expected.png"))?;
    write_png(frame, &artifacts.join("actual.png"))?;
    write_png(&diff, &artifacts.join("diff.png"))?;
    Ok(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    static NEXT_DIRECTORY: AtomicU64 = AtomicU64::new(0);

    struct TestDirectory(std::path::PathBuf);
    impl TestDirectory {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "rvm-capture-{}-{}",
                std::process::id(),
                NEXT_DIRECTORY.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir_all(&path).unwrap();
            Self(path)
        }
    }
    impl Drop for TestDirectory {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn colors() -> FrameSnapshot {
        FrameSnapshot {
            width: 4,
            height: 1,
            sequence: 12,
            pixels: vec![0xf800, 0x07e0, 0x001f, 0xffff],
        }
    }

    #[test]
    fn capture_has_native_dimensions_and_exact_channel_values() {
        let directory = TestDirectory::new();
        let png_path = directory.0.join("colors.png");
        let raw_path = directory.0.join("colors.rgb565");
        capture_frame(&colors(), Some(&png_path), Some(&raw_path)).unwrap();
        assert_eq!(
            fs::read(raw_path).unwrap(),
            vec![0, 248, 224, 7, 31, 0, 255, 255]
        );
        let decoder = png::Decoder::new(File::open(png_path).unwrap());
        let mut reader = decoder.read_info().unwrap();
        let mut output = vec![0; reader.output_buffer_size()];
        let info = reader.next_frame(&mut output).unwrap();
        assert_eq!((info.width, info.height), (4, 1));
        assert_eq!(info.color_type, png::ColorType::Rgb);
        assert_eq!(
            &output[..info.buffer_size()],
            &[255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255]
        );
        assert_eq!(rgb565_to_rgb888(0x8410), [132, 130, 132]);
    }

    #[test]
    fn exact_comparison_creates_diagnostics_only_for_mismatch() {
        let directory = TestDirectory::new();
        let expected = directory.0.join("expected.rgb565");
        let artifacts = directory.0.join("mismatch");
        write_raw(&colors(), &expected).unwrap();
        assert!(compare_raw(&colors(), &expected, &artifacts).unwrap());
        assert!(!artifacts.exists());
        let mut actual = colors();
        actual.pixels[0] ^= 1;
        assert!(!compare_raw(&actual, &expected, &artifacts).unwrap());
        assert!(artifacts.join("expected.png").exists());
        assert!(artifacts.join("actual.png").exists());
        assert!(artifacts.join("diff.png").exists());
    }

    #[test]
    fn malformed_frame_and_output_failures_are_errors() {
        let directory = TestDirectory::new();
        let mut frame = colors();
        frame.pixels.pop();
        assert!(capture_frame(&frame, Some(&directory.0.join("invalid.png")), None).is_err());
        assert!(capture_frame(&colors(), Some(&directory.0), None).is_err());
    }
}
