use pixels::{Pixels, SurfaceTexture};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Mutex,
};
use winit::dpi::LogicalSize;
use winit::event::{ElementState, Event, VirtualKeyCode, WindowEvent};
use winit::event_loop::{ControlFlow, EventLoopBuilder};
use winit::platform::run_return::EventLoopExtRunReturn;
use winit::window::WindowBuilder;

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

/// Shared display and host keyboard state. Device input is latched by the VM.
pub struct RGB565State {
    pub width: u16,
    pub height: u16,
    pub initialized: bool,
    pub front_buffer: Arc<Vec<u16>>,
    pub back_buffer: Vec<u16>,
    pub sequence: u64,
    shutdown: Arc<AtomicBool>,
    pub key_up: bool,
    pub key_down: bool,
    pub key_left: bool,
    pub key_right: bool,
    pub key_z: bool,
    pub key_x: bool,
    /// Stable USB HID keyboard usage IDs; OS auto-repeat is suppressed.
    pub pending_keys: Vec<(u16, bool)>,
    pub keys_held: [bool; 256],
    /// The VM must resynchronize from keys_held if this is set.
    pub key_overflow: bool,
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
            key_up: false,
            key_down: false,
            key_left: false,
            key_right: false,
            key_z: false,
            key_x: false,
            pending_keys: Vec::new(),
            keys_held: [false; 256],
            key_overflow: false,
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
                pixels_needed * 2, available_space * 2
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
        let Some(held) = self.keys_held.get_mut(key as usize) else {
            return;
        };
        if *held == down {
            return;
        }
        *held = down;
        self.key_up = self.keys_held[82];
        self.key_down = self.keys_held[81];
        self.key_left = self.keys_held[80];
        self.key_right = self.keys_held[79];
        self.key_z = self.keys_held[29];
        self.key_x = self.keys_held[27];
        if self.pending_keys.len() < 4096 && !self.key_overflow {
            self.pending_keys.push((key, down));
        } else {
            self.key_overflow = true;
        }
    }

    pub fn release_keys(&mut self) {
        for key in 0..256 {
            self.set_key(key, false);
        }
    }

    pub fn request_shutdown(&self) {
        self.shutdown.store(true, Ordering::Release);
    }

    pub fn should_quit(&self) -> bool {
        self.shutdown.load(Ordering::Acquire)
    }

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
struct FrameVersion {
    sequence: u64,
    layout_revision: u64,
}

struct SharedFrame {
    width: u16,
    height: u16,
    pixels: Arc<Vec<u16>>,
}

struct FrameUpdate {
    version: FrameVersion,
    frame: Option<SharedFrame>,
}

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

/// Convert host keys explicitly; no host enum discriminants are part of the ABI.
pub fn key_usage(key: VirtualKeyCode) -> Option<u16> {
    use VirtualKeyCode::*;
    Some(match key {
        A => 4,
        B => 5,
        C => 6,
        D => 7,
        E => 8,
        F => 9,
        G => 10,
        H => 11,
        I => 12,
        J => 13,
        K => 14,
        L => 15,
        M => 16,
        N => 17,
        O => 18,
        P => 19,
        Q => 20,
        R => 21,
        S => 22,
        T => 23,
        U => 24,
        V => 25,
        W => 26,
        X => 27,
        Y => 28,
        Z => 29,
        Key1 => 30,
        Key2 => 31,
        Key3 => 32,
        Key4 => 33,
        Key5 => 34,
        Key6 => 35,
        Key7 => 36,
        Key8 => 37,
        Key9 => 38,
        Key0 => 39,
        Return => 40,
        Escape => 41,
        Back => 42,
        Tab => 43,
        Space => 44,
        Minus => 45,
        Equals => 46,
        LBracket => 47,
        RBracket => 48,
        Backslash => 49,
        Semicolon => 51,
        Apostrophe => 52,
        Grave => 53,
        Comma => 54,
        Period => 55,
        Slash => 56,
        Capital => 57,
        F1 => 58,
        F2 => 59,
        F3 => 60,
        F4 => 61,
        F5 => 62,
        F6 => 63,
        F7 => 64,
        F8 => 65,
        F9 => 66,
        F10 => 67,
        F11 => 68,
        F12 => 69,
        Snapshot => 70,
        Scroll => 71,
        Pause => 72,
        Insert => 73,
        Home => 74,
        PageUp => 75,
        Delete => 76,
        End => 77,
        PageDown => 78,
        Right => 79,
        Left => 80,
        Down => 81,
        Up => 82,
        Numlock => 83,
        NumpadDivide => 84,
        NumpadMultiply => 85,
        NumpadSubtract => 86,
        NumpadAdd => 87,
        NumpadEnter => 88,
        Numpad1 => 89,
        Numpad2 => 90,
        Numpad3 => 91,
        Numpad4 => 92,
        Numpad5 => 93,
        Numpad6 => 94,
        Numpad7 => 95,
        Numpad8 => 96,
        Numpad9 => 97,
        Numpad0 => 98,
        NumpadDecimal => 99,
        OEM102 => 100,
        Apps => 101,
        Power => 102,
        NumpadEquals => 103,
        F13 => 104,
        F14 => 105,
        F15 => 106,
        F16 => 107,
        F17 => 108,
        F18 => 109,
        F19 => 110,
        F20 => 111,
        F21 => 112,
        F22 => 113,
        F23 => 114,
        F24 => 115,
        NumpadComma => 133,
        LControl => 224,
        LShift => 225,
        LAlt => 226,
        LWin => 227,
        RControl => 228,
        RShift => 229,
        RAlt => 230,
        RWin => 231,
        _ => return None,
    })
}

/// Run on the main thread, returning when the guest finishes or the window closes.
pub fn run_rgb565_display(
    state: Arc<Mutex<RGB565State>>,
) -> Result<(), Box<dyn std::error::Error>> {
    // Also stop the guest when window/GPU initialization returns an error.
    // Keep this guard outside the event-loop closure so unwinding releases it.
    struct StopGuest(Arc<AtomicBool>);
    impl Drop for StopGuest {
        fn drop(&mut self) {
            self.0.store(true, Ordering::Release);
        }
    }
    let shutdown = Arc::clone(&state.lock().unwrap().shutdown);
    let _stop_guest = StopGuest(Arc::clone(&shutdown));
    let mut event_loop = EventLoopBuilder::new().build();
    let window = WindowBuilder::new()
        .with_title("Ripple VM - RGB565 Display")
        .with_inner_size(LogicalSize::new(512u32, 512u32))
        .with_resizable(true)
        .build(&event_loop)?;
    let size = window.inner_size();
    // Some hosts report a zero initial size until their first resize event.
    // A minimized window may do the same. Pixels requires a nonzero backing
    // surface, but rendering stays suspended until the real size is usable.
    let (surface_width, surface_height) = backing_surface_size(size.width, size.height);
    let mut surface_visible = size.width > 0 && size.height > 0;
    let mut pixels = Pixels::new(
        256,
        256,
        SurfaceTexture::new(surface_width, surface_height, &window),
    )?;
    let mut dimensions = (256u32, 256u32);
    let mut rendered_version = None;
    let mut render_error = None;

    event_loop.run_return(|event, _, control_flow| {
        *control_flow = ControlFlow::WaitUntil(
            std::time::Instant::now() + std::time::Duration::from_millis(16),
        );
        if shutdown.load(Ordering::Acquire) {
            *control_flow = ControlFlow::Exit;
            return;
        }
        match event {
            Event::WindowEvent {
                event: WindowEvent::CloseRequested,
                ..
            } => {
                let mut shared = state.lock().unwrap();
                shared.release_keys();
                shared.request_shutdown();
                *control_flow = ControlFlow::Exit;
            }
            Event::WindowEvent {
                event: WindowEvent::Focused(false),
                ..
            } => {
                state.lock().unwrap().release_keys();
            }
            Event::WindowEvent {
                event: WindowEvent::KeyboardInput { input, .. },
                ..
            } => {
                if let Some(key) = input.virtual_keycode.and_then(key_usage) {
                    state
                        .lock()
                        .unwrap()
                        .set_key(key, input.state == ElementState::Pressed);
                }
            }
            Event::WindowEvent {
                event: WindowEvent::Resized(size),
                ..
            } => {
                surface_visible = size.width > 0 && size.height > 0;
                if surface_visible {
                    if let Err(error) = pixels.resize_surface(size.width, size.height) {
                        render_error = Some(error.to_string());
                        *control_flow = ControlFlow::Exit;
                    }
                }
            }
            Event::WindowEvent {
                event: WindowEvent::ScaleFactorChanged { new_inner_size, .. },
                ..
            } => {
                surface_visible = new_inner_size.width > 0 && new_inner_size.height > 0;
                if surface_visible {
                    if let Err(error) =
                        pixels.resize_surface(new_inner_size.width, new_inner_size.height)
                    {
                        render_error = Some(error.to_string());
                        *control_flow = ControlFlow::Exit;
                    }
                }
            }
            Event::RedrawRequested(_) => {
                if !surface_visible {
                    return;
                }
                // Clone just the immutable frame handle while locked. Color
                // conversion and resizing cannot block guest input/publication.
                let update = state.lock().unwrap().frame_update(rendered_version);
                if let Some(update) = update {
                    if let Some(frame) = &update.frame {
                        let requested = (frame.width as u32, frame.height as u32);
                        if requested != dimensions {
                            if let Err(error) = pixels.resize_buffer(requested.0, requested.1) {
                                render_error = Some(error.to_string());
                                *control_flow = ControlFlow::Exit;
                                return;
                            }
                            dimensions = requested;
                            window
                                .set_inner_size(LogicalSize::new(requested.0 * 2, requested.1 * 2));
                        }
                    }
                    update.write_rgba(pixels.frame_mut(), dimensions.0);
                    rendered_version = Some(update.version);
                }
                // Expose and surface-resize events still redraw the cached RGBA
                // image even when the guest has not presented another frame.
                if let Err(error) = pixels.render() {
                    render_error = Some(error.to_string());
                    *control_flow = ControlFlow::Exit;
                }
            }
            Event::MainEventsCleared => window.request_redraw(),
            _ => {}
        }
    });
    state.lock().unwrap().request_shutdown();
    if let Some(error) = render_error {
        return Err(error.into());
    }
    Ok(())
}

fn backing_surface_size(width: u32, height: u32) -> (u32, u32) {
    (width.max(1), height.max(1))
}

impl Drop for RGB565Display {
    fn drop(&mut self) {
        self.shutdown();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn initially_hidden_windows_always_have_a_nonzero_backing_surface() {
        assert_eq!(backing_surface_size(0, 0), (1, 1));
        assert_eq!(backing_surface_size(512, 0), (512, 1));
        assert_eq!(backing_surface_size(0, 512), (1, 512));
        assert_eq!(backing_surface_size(640, 400), (640, 400));
    }

    #[test]
    fn legacy_front_back_and_unclaimed_ram() {
        let mut display = RGB565Display::new();
        display.init(2, 1, 64).unwrap();
        assert!(display.write_memory(34, 0xf800));
        assert!(display.write_memory(35, 0x07e0));
        assert!(display.write_memory(32, 0xffff));
        assert_eq!(display.read_memory(32), Some(0));
        assert!(!display.write_memory(36, 123));
        assert!(display.flush());
        let snapshot = display.snapshot().unwrap();
        assert_eq!(snapshot.pixels, vec![0xf800, 0x07e0]);
        assert_eq!(snapshot.sequence, 1);
        display.write_memory(34, 0x001f);
        display.flush();
        assert_eq!(snapshot.pixels, vec![0xf800, 0x07e0]);
        assert_eq!(display.snapshot().unwrap().sequence, 2);
    }

    #[test]
    fn extended_frames_resize_and_modes_preserve_sequence() {
        let mut display = RGB565Display::new();
        let state = display.get_state();
        display.present(320, 200, vec![0x001f; 64000]).unwrap();
        assert_eq!(display.snapshot().unwrap().width, 320);
        assert_eq!(display.read_memory(32), None);
        assert!(!display.write_memory(32, 0xffff));
        assert!(!display.flush());
        display.disable();
        assert!(display.snapshot().is_none());
        assert!(!state.lock().unwrap().should_quit());
        display.init(1, 1, 34).unwrap();
        display.flush();
        assert_eq!(display.snapshot().unwrap().sequence, 2);
    }

    #[test]
    fn invalid_dimensions_do_not_publish() {
        let mut display = RGB565Display::new();
        assert!(display.init(0, 1, 64).is_err());
        assert!(display.init(1, 1, 31).is_err());
        assert!(display.init(2, 1, 35).is_err());
        assert!(display.present(2, 2, vec![0; 3]).is_err());
        assert!(display.present(0, 0, Vec::new()).is_err());
        assert!(display.snapshot().is_none());
    }

    #[test]
    fn rendered_frames_share_publication_and_skip_unchanged_pixels() {
        let mut display = RGB565Display::new();
        let pixels = vec![0xf800, 0x07e0, 0x001f, 0xffff];
        let allocation = pixels.as_ptr();
        display.present(2, 2, pixels).unwrap();
        let state = display.get_state();
        let update = state.lock().unwrap().frame_update(None).unwrap();
        assert_eq!(update.frame.as_ref().unwrap().pixels.as_ptr(), allocation);
        let mut rgba = vec![0; 16];
        update.write_rgba(&mut rgba, 2);
        assert_eq!(
            rgba,
            [255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255]
        );
        assert!(state
            .lock()
            .unwrap()
            .frame_update(Some(update.version))
            .is_none());

        display.present(1, 1, vec![0]).unwrap();
        let next = state
            .lock()
            .unwrap()
            .frame_update(Some(update.version))
            .unwrap();
        assert_eq!(next.frame.as_ref().unwrap().width, 1);
        assert_eq!(next.frame.as_ref().unwrap().height, 1);
        assert_eq!(next.version.sequence, 2);
        // The old view remains immutable while the guest publishes its next frame.
        update.write_rgba(&mut rgba, 2);
        assert_eq!(&rgba[..4], &[255, 0, 0, 255]);
    }

    #[test]
    fn mode_changes_invalidate_render_cache_without_presenting() {
        let mut display = RGB565Display::new();
        let state = display.get_state();
        let disabled = state.lock().unwrap().frame_update(None).unwrap();
        assert!(disabled.frame.is_none());
        assert!(state
            .lock()
            .unwrap()
            .frame_update(Some(disabled.version))
            .is_none());

        display.init(1, 1, 34).unwrap();
        let initialized = state
            .lock()
            .unwrap()
            .frame_update(Some(disabled.version))
            .unwrap();
        assert_eq!(initialized.version.sequence, 0);
        let mut rgba = [255; 4];
        initialized.write_rgba(&mut rgba, 1);
        assert_eq!(rgba, [0, 0, 0, 255]);

        // Reinitializing the same size also resets its visible front buffer.
        display.init(1, 1, 34).unwrap();
        let reinitialized = state
            .lock()
            .unwrap()
            .frame_update(Some(initialized.version))
            .unwrap();
        assert_eq!(reinitialized.version.sequence, 0);
        display.disable();
        let disabled = state
            .lock()
            .unwrap()
            .frame_update(Some(reinitialized.version))
            .unwrap();
        assert_eq!(disabled.version.sequence, 0);
        assert!(disabled.frame.is_none());
        disabled.write_rgba(&mut rgba, 1);
        assert_eq!(rgba, [64, 64, 64, 255]);
        assert!(!display.is_window_closed());
    }

    #[test]
    fn legacy_flush_preserves_a_frame_being_converted() {
        let mut display = RGB565Display::new();
        display.init(2, 1, 64).unwrap();
        display.write_memory(34, 0xf800);
        display.write_memory(35, 0x07e0);
        display.flush();
        let state = display.get_state();
        let previous = state.lock().unwrap().frame_update(None).unwrap();
        display.write_memory(34, 0x001f);
        // Back-buffer writes do not make a new frame visible.
        assert!(state
            .lock()
            .unwrap()
            .frame_update(Some(previous.version))
            .is_none());
        display.flush();
        let next = state
            .lock()
            .unwrap()
            .frame_update(Some(previous.version))
            .unwrap();
        assert_eq!(next.frame.as_ref().unwrap().pixels.as_slice(), &[0x001f, 0]);
        assert_eq!(display.read_memory(34), Some(0xf800));
        // Reusing the old front as guest back RAM must not change its UI view.
        display.write_memory(34, 0xffff);
        assert_eq!(
            previous.frame.as_ref().unwrap().pixels.as_slice(),
            &[0xf800, 0x07e0]
        );
        let mut rgba = [0; 8];
        previous.write_rgba(&mut rgba, 2);
        assert_eq!(rgba, [255, 0, 0, 255, 0, 255, 0, 255]);
    }

    #[test]
    fn shutdown_poll_does_not_acquire_the_frame_mutex() {
        let mut display = RGB565Display::new();
        let state = display.get_state();
        let locked = state.lock().unwrap();
        assert!(!display.is_window_closed());
        locked.request_shutdown();
        assert!(locked.should_quit());
        assert!(display.is_window_closed());
        // Shutdown and Drop also remain safe while another owner has the lock.
        display.shutdown();
        drop(display);
        assert!(locked.should_quit());
    }

    #[test]
    fn shutdown_and_drop_signal_the_shared_connection() {
        let mut display = RGB565Display::new();
        let state = display.get_state();
        display.shutdown();
        assert!(display.is_window_closed());
        assert!(state.lock().unwrap().should_quit());
        let display = RGB565Display::new();
        let state = display.get_state();
        drop(display);
        let state = state.lock().unwrap();
        assert!(state.should_quit());
        assert!(state.shutdown.load(Ordering::Acquire));
    }

    #[test]
    fn keyboard_transitions_suppress_repeat_and_release_on_focus_loss() {
        let mut state = RGB565State::new();
        state.set_key(82, true);
        state.set_key(82, true);
        state.set_key(29, true);
        assert!(state.key_up && state.key_z);
        state.release_keys();
        assert_eq!(
            state.pending_keys,
            vec![(82, true), (29, true), (29, false), (82, false)]
        );
        assert!(state.keys_held.iter().all(|held| !held));
        assert!(!state.key_up && !state.key_z);
        assert_eq!(key_usage(VirtualKeyCode::F12), Some(69));
        assert_eq!(key_usage(VirtualKeyCode::RControl), Some(228));
    }

    #[test]
    fn input_overflow_preserves_held_state() {
        let mut state = RGB565State::new();
        for index in 0..4097 {
            state.set_key(4, index % 2 == 0);
        }
        assert!(state.key_overflow);
        assert_eq!(state.pending_keys.len(), 4096);
        assert!(state.keys_held[4]);
    }
}
