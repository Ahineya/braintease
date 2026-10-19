use crate::input_events::{InputState, InputTransition};
use serde::{Deserialize, Serialize};
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
};

/// Expand RGB565 channels by repeating their high bits, without rounding drift.
pub fn rgb565_to_rgb888(color: u16) -> [u8; 3] {
    let r = ((color >> 11) & 0x1f) as u8;
    let g = ((color >> 5) & 0x3f) as u8;
    let b = (color & 0x1f) as u8;
    [
        (r << 3) | (r >> 2),
        (g << 2) | (g >> 4),
        (b << 3) | (b >> 2),
    ]
}

/// A copy of a completed frame, independent of subsequent guest writes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FrameSnapshot {
    pub width: u16,
    pub height: u16,
    pub sequence: u64,
    pub pixels: Vec<u16>,
}

/// Persistent display buffers and queued input, separate from the window/GPU.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RGB565Checkpoint {
    width: u16,
    height: u16,
    pub initialized: bool,
    front: Vec<u16>,
    back: Vec<u16>,
    sequence: u64,
    input: InputState,
    pub legacy_layout: bool,
    layout_revision: u64,
}

/// Shared display and host keyboard state. Hosts collect queued transitions on input polls.
pub struct RGB565State {
    pub width: u16,
    pub height: u16,
    pub initialized: bool,
    pub front_buffer: Arc<Vec<u16>>,
    pub back_buffer: Vec<u16>,
    pub sequence: u64,
    shutdown: Arc<AtomicBool>,
    pub input: InputState,
    legacy_layout: bool,
    // Initialization and mode changes can replace pixels without presenting a
    // frame, so sequence alone is not enough to invalidate the window cache.
    layout_revision: u64,
}

impl Default for RGB565State {
    fn default() -> Self {
        Self::new()
    }
}

impl RGB565State {
    pub fn new() -> Self {
        Self {
            width: 0,
            height: 0,
            initialized: false,
            front_buffer: Arc::new(Vec::new()),
            back_buffer: Vec::new(),
            sequence: 0,
            shutdown: Arc::new(AtomicBool::new(false)),
            input: InputState::default(),
            legacy_layout: false,
            layout_revision: 0,
        }
    }

    pub fn init(&mut self, width: u8, height: u8, bank_size: usize) -> Result<(), String> {
        if width == 0 || height == 0 {
            return Err("Display dimensions must be nonzero".to_string());
        }
        let pixels_needed = width as usize * height as usize;
        let available_space = bank_size.checked_sub(32).unwrap_or(0) / 2;
        if bank_size < 32 || pixels_needed > available_space {
            return Err(format!(
                "Display resolution {width}x{height} requires {} cells, but only {} available in bank",
                pixels_needed * 2,
                available_space * 2
            ));
        }
        self.width = width as u16;
        self.height = height as u16;
        self.front_buffer = Arc::new(vec![0; pixels_needed]);
        self.back_buffer = vec![0; pixels_needed];
        self.initialized = true;
        self.legacy_layout = true;
        self.layout_revision = self.layout_revision.wrapping_add(1);
        Ok(())
    }

    pub fn swap_buffers(&mut self) -> bool {
        if !self.initialized || !self.legacy_layout {
            return false;
        }
        let Some(next_sequence) = self.sequence.checked_add(1) else {
            return false;
        };
        let next_front = Arc::new(std::mem::take(&mut self.back_buffer));
        let previous_front = std::mem::replace(&mut self.front_buffer, next_front);
        // Usually the window has finished reading the old front by now. If it
        // has not, preserve its immutable view before reusing this as guest RAM.
        self.back_buffer =
            Arc::try_unwrap(previous_front).unwrap_or_else(|shared| shared.as_ref().clone());
        self.sequence = next_sequence;
        true
    }

    /// Record a transition, preserving authoritative held state even on overflow.
    pub fn set_key(&mut self, key: u16, down: bool) {
        self.input.apply(InputTransition::Key { key, down }, 0);
    }
    /// Synthetic host events can carry modifier flags without physical modifier
    /// key transitions. Recover an aggregate left-key alias in that case.
    #[cfg(not(target_arch = "wasm32"))]
    fn sync_modifiers(&mut self, modifiers: winit::event::ModifiersState) {
        for (left, right, pressed) in [
            (224, 228, modifiers.ctrl()),
            (225, 229, modifiers.shift()),
            (226, 230, modifiers.alt()),
            (227, 231, modifiers.logo()),
        ] {
            if !pressed {
                self.set_key(left, false);
                self.set_key(right, false);
            } else if !self.input.held(left) && !self.input.held(right) {
                self.set_key(left, true);
            }
        }
    }
    pub fn release_keys(&mut self) {
        for key in 0..256 {
            self.set_key(key, false);
        }
    }
    pub fn input_event(&mut self, event: InputTransition) {
        self.input.apply(event, 0);
    }

    pub fn request_shutdown(&self) {
        self.shutdown.store(true, Ordering::Release);
    }

    pub fn should_quit(&self) -> bool {
        self.shutdown.load(Ordering::Acquire)
    }

    #[cfg(not(target_arch = "wasm32"))]
    fn frame_update(&self, previous: Option<FrameVersion>) -> Option<FrameUpdate> {
        let version = FrameVersion {
            sequence: self.sequence,
            layout_revision: self.layout_revision,
        };
        if previous == Some(version) {
            return None;
        }
        Some(FrameUpdate {
            version,
            frame: self.initialized.then(|| SharedFrame {
                width: self.width,
                height: self.height,
                pixels: Arc::clone(&self.front_buffer),
            }),
        })
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[cfg(not(target_arch = "wasm32"))]
struct FrameVersion {
    sequence: u64,
    layout_revision: u64,
}

#[cfg(not(target_arch = "wasm32"))]
struct SharedFrame {
    width: u16,
    height: u16,
    pixels: Arc<Vec<u16>>,
}

#[cfg(not(target_arch = "wasm32"))]
struct FrameUpdate {
    version: FrameVersion,
    frame: Option<SharedFrame>,
}

#[cfg(not(target_arch = "wasm32"))]
impl FrameUpdate {
    fn write_rgba(&self, output: &mut [u8], width: u32) {
        if let Some(frame) = &self.frame {
            for (output, color) in output.chunks_exact_mut(4).zip(frame.pixels.iter()) {
                let [r, g, b] = rgb565_to_rgb888(*color);
                output.copy_from_slice(&[r, g, b, 255]);
            }
        } else {
            for (index, pixel) in output.chunks_exact_mut(4).enumerate() {
                let x = index as u32 % width;
                let y = index as u32 / width;
                let color = if ((x / 16) + (y / 16)) % 2 == 0 {
                    64
                } else {
                    32
                };
                pixel.copy_from_slice(&[color, color, color, 255]);
            }
        }
    }
}

pub struct RGB565Display {
    state: Arc<Mutex<RGB565State>>,
    shutdown: Arc<AtomicBool>,
}

impl Default for RGB565Display {
    fn default() -> Self {
        Self::new()
    }
}

impl RGB565Display {
    pub fn checkpoint(&self) -> Result<RGB565Checkpoint, String> {
        let state = self
            .state
            .lock()
            .map_err(|_| "display state lock poisoned")?;
        Ok(RGB565Checkpoint {
            width: state.width,
            height: state.height,
            initialized: state.initialized,
            front: state.front_buffer.as_ref().clone(),
            back: state.back_buffer.clone(),
            sequence: state.sequence,
            input: state.input.clone(),
            legacy_layout: state.legacy_layout,
            layout_revision: state.layout_revision,
        })
    }

    pub fn restore_checkpoint(
        snapshot: RGB565Checkpoint,
        bank_size: usize,
    ) -> Result<Self, String> {
        let pixels = usize::from(snapshot.width) * usize::from(snapshot.height);
        if !snapshot.input.validate(0)
            || snapshot.initialized && (pixels == 0 || snapshot.front.len() != pixels)
            || !snapshot.initialized
                && (snapshot.width != 0
                    || snapshot.height != 0
                    || !snapshot.front.is_empty()
                    || !snapshot.back.is_empty()
                    || snapshot.legacy_layout)
            || snapshot.legacy_layout
                && (snapshot.width > 255
                    || snapshot.height > 255
                    || snapshot.back.len() != pixels
                    || pixels > bank_size.saturating_sub(32) / 2)
            || !snapshot.legacy_layout && !snapshot.back.is_empty()
        {
            return Err("invalid RGB565 checkpoint".into());
        }
        let mut state = RGB565State::new();
        state.width = snapshot.width;
        state.height = snapshot.height;
        state.initialized = snapshot.initialized;
        state.front_buffer = Arc::new(snapshot.front);
        state.back_buffer = snapshot.back;
        state.sequence = snapshot.sequence;
        state.input = snapshot.input;
        state.legacy_layout = snapshot.legacy_layout;
        state.layout_revision = snapshot.layout_revision;
        // A stopped host connection is not a stopped guest. The new window
        // receives a new shutdown token and redraws from the restored buffers.
        let shutdown = Arc::clone(&state.shutdown);
        Ok(Self {
            state: Arc::new(Mutex::new(state)),
            shutdown,
        })
    }

    pub fn new() -> Self {
        let state = RGB565State::new();
        let shutdown = Arc::clone(&state.shutdown);
        Self {
            state: Arc::new(Mutex::new(state)),
            shutdown,
        }
    }

    pub fn get_state(&self) -> Arc<Mutex<RGB565State>> {
        Arc::clone(&self.state)
    }

    pub fn is_window_closed(&self) -> bool {
        self.shutdown.load(Ordering::Acquire)
    }

    pub fn init(&mut self, width: u8, height: u8, bank_size: usize) -> Result<(), String> {
        self.state.lock().unwrap().init(width, height, bank_size)
    }

    /// Claim only the legacy framebuffer range; writes to its front are ignored.
    pub fn write_memory(&mut self, addr: usize, value: u16) -> bool {
        let mut state = self.state.lock().unwrap();
        if !state.initialized || !state.legacy_layout {
            return false;
        }
        let size = state.front_buffer.len();
        if (32..32 + size).contains(&addr) {
            return true;
        }
        if (32 + size..32 + size * 2).contains(&addr) {
            state.back_buffer[addr - 32 - size] = value;
            return true;
        }
        false
    }

    pub fn read_memory(&self, addr: usize) -> Option<u16> {
        let state = self.state.lock().unwrap();
        if !state.initialized || !state.legacy_layout {
            return None;
        }
        let size = state.front_buffer.len();
        if (32..32 + size).contains(&addr) {
            Some(state.front_buffer[addr - 32])
        } else if (32 + size..32 + size * 2).contains(&addr) {
            Some(state.back_buffer[addr - 32 - size])
        } else {
            None
        }
    }

    /// A legacy flush publishes exactly one completed frame.
    pub fn flush(&mut self) -> bool {
        self.state.lock().unwrap().swap_buffers()
    }

    /// Publish an RGB565_EXT buffer already copied and validated by the VM.
    pub fn present(&mut self, width: u16, height: u16, pixels: Vec<u16>) -> Result<(), String> {
        if width == 0 || height == 0 || pixels.len() != width as usize * height as usize {
            return Err("Frame dimensions do not match its pixel buffer".to_string());
        }
        let mut state = self.state.lock().unwrap();
        let next_sequence = state
            .sequence
            .checked_add(1)
            .ok_or_else(|| "Frame sequence overflow".to_string())?;
        state.width = width;
        state.height = height;
        // Arc<Vec<_>> moves the existing allocation; it does not copy pixels.
        state.front_buffer = Arc::new(pixels);
        state.back_buffer.clear();
        state.initialized = true;
        state.legacy_layout = false;
        state.sequence = next_sequence;
        Ok(())
    }

    pub fn snapshot(&self) -> Option<FrameSnapshot> {
        let state = self.state.lock().unwrap();
        if !state.initialized {
            return None;
        }
        Some(FrameSnapshot {
            width: state.width,
            height: state.height,
            sequence: state.sequence,
            pixels: state.front_buffer.as_ref().clone(),
        })
    }

    /// Keep the shared host/window connection alive when switching display mode.
    pub fn disable(&mut self) {
        let mut state = self.state.lock().unwrap();
        state.initialized = false;
        state.legacy_layout = false;
        state.width = 0;
        state.height = 0;
        state.front_buffer = Arc::new(Vec::new());
        state.back_buffer.clear();
        state.layout_revision = state.layout_revision.wrapping_add(1);
    }

    pub fn shutdown(&mut self) {
        self.shutdown.store(true, Ordering::Release);
    }
}


#[cfg(not(target_arch = "wasm32"))]
#[path = "display_rgb565_window.rs"]
mod window;
#[cfg(not(target_arch = "wasm32"))]
pub use window::*;
