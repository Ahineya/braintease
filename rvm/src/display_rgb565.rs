use crate::input_events::{InputState, InputTransition, POINTER_RELATIVE};
use pixels::{Pixels, SurfaceTexture};
use serde::{Deserialize, Serialize};
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
};
use winit::dpi::LogicalSize;
use winit::event::{
    DeviceEvent, ElementState, Event, ModifiersState, MouseButton, MouseScrollDelta,
    VirtualKeyCode, WindowEvent,
};
use winit::event_loop::{ControlFlow, EventLoopBuilder};
use winit::platform::run_return::EventLoopExtRunReturn;
use winit::window::CursorGrabMode;
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
    fn sync_modifiers(&mut self, modifiers: ModifiersState) {
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

/// A close suppresses the current lease, while an idle display merely waits
/// for its first frame. These must remain distinct when the host hides a window.
struct WindowVisibility {
    shown: bool,
    closed_layout: Option<u64>,
}
impl WindowVisibility {
    fn close(&mut self, revision: u64) {
        self.shown = false;
        self.closed_layout = Some(revision);
    }
    fn update(&mut self, initialized: bool, revision: u64) -> Option<bool> {
        if !initialized {
            self.closed_layout = None;
        }
        let show = initialized && self.closed_layout != Some(revision);
        let changed = self.shown != show;
        self.shown = show;
        changed.then_some(show)
    }
}

/// Run on the main thread, returning when the guest finishes or the window closes.
pub fn run_rgb565_display(
    state: Arc<Mutex<RGB565State>>,
) -> Result<(), Box<dyn std::error::Error>> {
    run_rgb565_display_with_close_policy(state, false)
}

/// In guest-close mode, hide a closed window and wait for a new display lease.
pub fn run_rgb565_display_with_close_policy(
    state: Arc<Mutex<RGB565State>>,
    guest_window_close: bool,
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
        .with_visible(!guest_window_close)
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
    let mut cursor_grabbed = false;
    let mut relative_remainder = (0.0, 0.0);
    let mut visibility_state = WindowVisibility {
        shown: !guest_window_close,
        closed_layout: None,
    };

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
                if guest_window_close {
                    visibility_state.close(shared.layout_revision);
                    shared.input_event(InputTransition::Close);
                    drop(shared);
                    window.set_visible(false);
                } else {
                    shared.input_event(InputTransition::Focus { focused: false });
                    shared.request_shutdown();
                    *control_flow = ControlFlow::Exit;
                }
            }
            Event::WindowEvent {
                event: WindowEvent::Focused(focused),
                ..
            } => {
                state
                    .lock()
                    .unwrap()
                    .input_event(InputTransition::Focus { focused });
            }
            Event::WindowEvent {
                event: WindowEvent::CursorMoved { position, .. },
                ..
            } => {
                let (x, y) =
                    match pixels.window_pos_to_pixel((position.x as f32, position.y as f32)) {
                        Ok((x, y)) => (x as i32, y as i32),
                        Err((x, y)) => (x as i32, y as i32),
                    };
                let mut shared = state.lock().unwrap();
                if !shared.input.captured {
                    shared.input_event(InputTransition::Motion { x, y });
                }
            }
            Event::DeviceEvent {
                event: DeviceEvent::MouseMotion { delta },
                ..
            } => {
                let mut shared = state.lock().unwrap();
                if shared.input.captured {
                    relative_remainder.0 += delta.0;
                    relative_remainder.1 += delta.1;
                    let (dx, dy) = (
                        relative_remainder.0.trunc() as i32,
                        relative_remainder.1.trunc() as i32,
                    );
                    relative_remainder.0 -= f64::from(dx);
                    relative_remainder.1 -= f64::from(dy);
                    shared.input_event(InputTransition::Relative { dx, dy });
                }
            }
            Event::WindowEvent {
                event:
                    WindowEvent::MouseInput {
                        state: pressed,
                        button,
                        ..
                    },
                ..
            } => {
                let button = match button {
                    MouseButton::Left => 0,
                    MouseButton::Right => 1,
                    MouseButton::Middle => 2,
                    MouseButton::Other(n) => extra_button(n),
                };
                let mut shared = state.lock().unwrap();
                let (x, y) = (shared.input.x, shared.input.y);
                shared.input_event(InputTransition::Button {
                    button,
                    down: pressed == ElementState::Pressed,
                    x,
                    y,
                });
            }
            Event::WindowEvent {
                event: WindowEvent::MouseWheel { delta, .. },
                ..
            } => {
                // Pixel-scroll deltas use an explicit 40 logical window pixels per line.
                let (x, y) = match delta {
                    MouseScrollDelta::LineDelta(x, y) => (f64::from(x), f64::from(y)),
                    MouseScrollDelta::PixelDelta(p) => (
                        p.x / window.scale_factor() / 40.0,
                        p.y / window.scale_factor() / 40.0,
                    ),
                };
                state.lock().unwrap().input_event(InputTransition::Wheel {
                    x: (x * 65536.0).round() as i32,
                    y: (y * 65536.0).round() as i32,
                });
            }
            Event::WindowEvent {
                event: WindowEvent::CursorEntered { .. },
                ..
            } => state
                .lock()
                .unwrap()
                .input_event(InputTransition::Enter { inside: true }),
            Event::WindowEvent {
                event: WindowEvent::CursorLeft { .. },
                ..
            } => state
                .lock()
                .unwrap()
                .input_event(InputTransition::Enter { inside: false }),
            Event::WindowEvent {
                event: WindowEvent::ModifiersChanged(modifiers),
                ..
            } => {
                state.lock().unwrap().sync_modifiers(modifiers);
            }
            Event::WindowEvent {
                event: WindowEvent::KeyboardInput { input, .. },
                ..
            } => {
                if let Some(key) = input.virtual_keycode.and_then(key_usage) {
                    let mut shared = state.lock().unwrap();
                    #[allow(deprecated)]
                    shared.sync_modifiers(input.modifiers);
                    shared.set_key(key, input.state == ElementState::Pressed);
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
                    window.request_redraw();
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
                    window.request_redraw();
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
                            let (width, height) = initial_window_size(frame.width, frame.height);
                            window.set_inner_size(LogicalSize::new(width, height));
                        }
                    }
                    update.write_rgba(pixels.frame_mut(), dimensions.0);
                    rendered_version = Some(update.version);
                }
                if !visibility_state.shown {
                    return;
                }
                // Expose and surface-resize events still redraw the cached RGBA
                // image even when the guest has not presented another frame.
                if let Err(error) = pixels.render() {
                    render_error = Some(error.to_string());
                    *control_flow = ControlFlow::Exit;
                }
            }
            Event::MainEventsCleared => {
                let mut shared = state.lock().unwrap();
                let frame_changed = shared.frame_update(rendered_version).is_some();
                // Hidden macOS windows may receive no redraw events. Reopening
                // must also be driven from the event loop's polling boundary.
                let visibility = if guest_window_close {
                    visibility_state.update(shared.initialized, shared.layout_revision)
                } else {
                    None
                };
                let want = shared.input.pointer_mode == POINTER_RELATIVE && shared.input.focused;
                if want != cursor_grabbed {
                    let captured = if want {
                        window
                            .set_cursor_grab(CursorGrabMode::Locked)
                            .or_else(|_| window.set_cursor_grab(CursorGrabMode::Confined))
                            .is_ok()
                    } else {
                        let _ = window.set_cursor_grab(CursorGrabMode::None);
                        false
                    };
                    cursor_grabbed = captured;
                    relative_remainder = (0.0, 0.0);
                    window.set_cursor_visible(!captured);
                    shared.input_event(InputTransition::Capture { captured });
                }
                drop(shared);
                if let Some(visible) = visibility {
                    window.set_visible(visible);
                }
                if frame_changed || visibility == Some(true) {
                    window.request_redraw();
                }
            }
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

/// Keep tiny framebuffers readable while retaining an integer pixel scale.
/// Larger framebuffers use one logical point per guest pixel. In particular,
/// 640x480 must fit a laptop desktop; Retina backing pixels are host-managed.
fn initial_window_size(width: u16, height: u16) -> (u32, u32) {
    let width = u32::from(width);
    let height = u32::from(height);
    let short_edge = width.min(height).max(1);
    let scale = 240u32.div_ceil(short_edge).max(1);
    (width * scale, height * scale)
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
    fn modifier_flags_supply_missing_transitions_and_release_families() {
        let display = RGB565Display::new();
        let shared = display.get_state();
        let mut host = shared.lock().unwrap();
        host.sync_modifiers(ModifiersState::CTRL | ModifiersState::SHIFT);
        assert!(host.input.held(224) && host.input.held(225));
        host.set_key(29, true);
        assert!(
            host.input
                .events
                .iter()
                .position(|e| e.code == 224)
                .unwrap()
                < host.input.events.iter().position(|e| e.code == 29).unwrap()
        );
        host.sync_modifiers(ModifiersState::empty());
        assert!(!host.input.held(224) && !host.input.held(225));
        host.set_key(230, true);
        host.sync_modifiers(ModifiersState::ALT);
        assert!(host.input.held(230) && !host.input.held(226));
    }

    #[test]
    fn guest_window_visibility_tracks_first_frame_close_and_new_lease() {
        let mut s = WindowVisibility {
            shown: false,
            closed_layout: None,
        };
        assert_eq!(s.update(false, 0), None);
        assert_eq!(s.update(false, 0), None);
        assert_eq!(s.update(true, 0), Some(true));
        assert_eq!(s.update(true, 0), None);
        s.close(0);
        assert_eq!(s.update(true, 0), None);
        assert_eq!(s.update(false, 1), None);
        assert_eq!(s.update(true, 1), Some(true));
        s.close(1);
        // The guest can replace its lease between two host event-loop turns.
        assert_eq!(s.update(true, 2), Some(true));
        assert_eq!(s.update(false, 3), Some(false));
        assert_eq!(s.update(false, 3), None);
    }

    #[test]
    fn checkpoint_preserves_both_legacy_buffers_and_pending_keyboard_input() {
        let mut display = RGB565Display::new();
        display.init(2, 1, 64).unwrap();
        display.write_memory(34, 0xabcd);
        display.flush();
        display.write_memory(35, 0x1234);
        display.get_state().lock().unwrap().set_key(82, true);
        let checkpoint = display.checkpoint().unwrap();
        let encoded = serde_json::to_vec(&checkpoint).unwrap();
        let mut restored =
            RGB565Display::restore_checkpoint(serde_json::from_slice(&encoded).unwrap(), 64)
                .unwrap();
        assert_eq!(restored.snapshot(), display.snapshot());
        assert_eq!(restored.read_memory(35), Some(0x1234));
        assert_eq!(
            restored
                .get_state()
                .lock()
                .unwrap()
                .input
                .events
                .iter()
                .map(|e| (e.code, e.flags & 1 != 0))
                .collect::<Vec<_>>(),
            [(82, true)]
        );
        display.flush();
        restored.flush();
        assert_eq!(restored.snapshot(), display.snapshot());
        assert_eq!(restored.read_memory(34), display.read_memory(34));
        let mut corrupt = checkpoint;
        corrupt.back.pop();
        assert!(RGB565Display::restore_checkpoint(corrupt, 64).is_err());
    }

    #[test]
    fn checkpoint_restores_extended_frame_with_a_fresh_window_connection() {
        let mut display = RGB565Display::new();
        display.present(3, 2, vec![1, 2, 3, 4, 5, 6]).unwrap();
        display.shutdown();
        let restored =
            RGB565Display::restore_checkpoint(display.checkpoint().unwrap(), 64).unwrap();
        assert_eq!(restored.snapshot(), display.snapshot());
        assert!(!restored.is_window_closed());
        assert_eq!(restored.read_memory(32), None);
    }

    #[test]
    fn initially_hidden_windows_always_have_a_nonzero_backing_surface() {
        assert_eq!(backing_surface_size(0, 0), (1, 1));
        assert_eq!(backing_surface_size(512, 0), (512, 1));
        assert_eq!(backing_surface_size(0, 512), (1, 512));
        assert_eq!(backing_surface_size(640, 400), (640, 400));
    }

    #[test]
    fn small_frames_open_at_a_readable_integer_scale() {
        assert_eq!(initial_window_size(16, 12), (320, 240));
        assert_eq!(initial_window_size(12, 16), (240, 320));
        assert_eq!(initial_window_size(64, 64), (256, 256));
        assert_eq!(initial_window_size(1, 1), (240, 240));
        assert_eq!(initial_window_size(160, 100), (480, 300));
        assert_eq!(initial_window_size(320, 200), (640, 400));
        assert_eq!(initial_window_size(256, 256), (256, 256));
        assert_eq!(initial_window_size(640, 480), (640, 480));
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
            [
                255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255
            ]
        );
        assert!(
            state
                .lock()
                .unwrap()
                .frame_update(Some(update.version))
                .is_none()
        );

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
        assert!(
            state
                .lock()
                .unwrap()
                .frame_update(Some(disabled.version))
                .is_none()
        );

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
        assert!(
            state
                .lock()
                .unwrap()
                .frame_update(Some(previous.version))
                .is_none()
        );
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
        assert!(state.input.held(82) && state.input.held(29));
        state.release_keys();
        assert_eq!(
            state
                .input
                .events
                .iter()
                .map(|e| (e.code, e.flags & 1 != 0))
                .collect::<Vec<_>>(),
            vec![(82, true), (29, true), (29, false), (82, false)]
        );
        assert!(state.input.held.iter().all(|held| *held == 0));
        assert!(!state.input.held(82) && !state.input.held(29));
        assert_eq!(key_usage(VirtualKeyCode::F12), Some(69));
        assert_eq!(key_usage(VirtualKeyCode::RControl), Some(228));
    }

    #[test]
    fn input_overflow_preserves_held_state() {
        let mut state = RGB565State::new();
        for index in 0..4097 {
            state.set_key(4, index % 2 == 0);
        }
        assert!(state.input.overflow);
        assert_eq!(
            state
                .input
                .events
                .iter()
                .map(|e| (e.code, e.flags & 1 != 0))
                .collect::<Vec<_>>()
                .len(),
            256
        );
        assert!(state.input.held(4));
    }
}

// Winit preserves platform button numbers for auxiliary buttons.
fn extra_button(n: u16) -> u16 {
    #[cfg(target_os = "windows")]
    {
        n.saturating_add(2)
    }
    #[cfg(target_os = "linux")]
    {
        if n >= 275 { n - 272 } else { n }
    }
    #[cfg(not(any(target_os = "windows", target_os = "linux")))]
    {
        n
    }
}
