//! Hosted GX windows. Only owned pixels/events cross the guest/UI boundary.
//! The kernel checks process ownership; this adapter checks handles and spans.
use super::*;
use rvm::input_events::{InputState, InputTransition};
use std::sync::{Arc, Mutex};

const MAX_WINDOWS: usize = 16;
const MAX_WIDTH: u32 = 1024;
const MAX_HEIGHT: u32 = 768;
type Result<T> = std::result::Result<T, u16>;

pub(super) struct Surface {
    width: u32,
    height: u32,
    title: String,
    back: Vec<u16>,
    front: Arc<Vec<u16>>,
    sequence: u64,
    input: InputState,
    closed: bool,
    flags: u32,
    resize: Option<(u32, u32)>,
    activation: u64,
    configuration: u64,
}
impl Surface {
    fn transition(&mut self, event: InputTransition) {
        if matches!(event, InputTransition::Close) {
            if self.closed {
                return;
            }
            self.closed = true;
        }
        self.input.apply(event, 0);
    }
    fn request_resize(&mut self, width: u32, height: u32) {
        if self.flags & 1 == 0
            && width > 0
            && height > 0
            && (width, height) != (self.width, self.height)
        {
            self.resize = Some((width.min(MAX_WIDTH), height.min(MAX_HEIGHT)));
        }
    }
}
#[derive(Default)]
pub(super) struct Shared {
    windows: BTreeMap<u32, Surface>,
    next: u32,
    pub(super) finished: bool,
    pub(super) new_terminals: u16,
}
#[derive(Default)]
pub(super) struct Graphics {
    pub(super) shared: Arc<Mutex<Shared>>,
}
fn span(ptr: u32, count: usize) -> Result<usize> {
    let bank = (ptr >> 16) as usize;
    let offset = (ptr & 65535) as usize;
    if bank >= 256 || offset >= BANK || ptr == 0 {
        return Err(14);
    }
    let start = bank * BANK + offset;
    if start.checked_add(count).is_none_or(|n| n > CELLS) {
        return Err(14);
    }
    Ok(start)
}
fn title(rt: &Runtime, ptr: u32) -> Result<String> {
    let start = span(ptr, 64)?;
    let mut bytes = Vec::new();
    for i in 0..64 {
        let c = rt.memory.read(start + i);
        if c == 0 {
            return Ok(String::from_utf8_lossy(&bytes).into_owned());
        }
        if c > 255 {
            return Err(22);
        }
        bytes.push(c as u8);
    }
    Err(22)
}
fn write_cells(rt: &mut Runtime, ptr: u32, cells: &[u16]) -> Result<()> {
    let start = span(ptr, cells.len())?;
    for (i, c) in cells.iter().enumerate() {
        rt.memory.write(start + i, *c);
    }
    Ok(())
}
fn pair(out: &mut Vec<u16>, n: u32) {
    out.extend([n as u16, (n >> 16) as u16]);
}
// Optional acceptance-test captures use the same immutable published frames.
// Atomic replacement lets the external test observe a coherent registry.
fn capture(shared: &Shared) -> Result<()> {
    let Some(directory) = std::env::var_os("FUSE_X_FRAMES") else {
        return Ok(());
    };
    let directory = std::path::Path::new(&directory);
    std::fs::create_dir_all(directory).map_err(|_| 5u16)?;
    let windows:Vec<_>=shared.windows.iter().map(|(&id,w)|serde_json::json!({
        "id":id,"title":w.title,"width":w.width,"height":w.height,"visible":w.flags&2==0,"resizable":w.flags&1==0,"sequence":w.sequence
    })).collect();
    let temporary = directory.join("windows.json.tmp");
    std::fs::write(&temporary, serde_json::to_vec(&windows).unwrap()).map_err(|_| 5u16)?;
    std::fs::rename(temporary, directory.join("windows.json")).map_err(|_| 5u16)?;
    Ok(())
}
impl Runtime {
    fn graphics_call(&mut self, op: u64, a: &mut [u32; 12]) -> Result<u32> {
        let shared = self.graphics.shared.clone();
        if op == 11 {
            let mut s = shared.lock().unwrap();
            let ready = s.new_terminals != 0;
            s.new_terminals = s.new_terminals.saturating_sub(1);
            return Ok(u32::from(ready));
        }
        if op == 1 {
            let (w, h) = (a[0], a[1]);
            if w == 0 || h == 0 || w > MAX_WIDTH || h > MAX_HEIGHT {
                return Err(22);
            }
            let title = title(self, a[2])?;
            let mut s = shared.lock().unwrap();
            if s.windows.len() >= MAX_WINDOWS {
                return Err(12);
            }
            let id = s.next.checked_add(1).ok_or(12u16)?;
            s.next = id;
            let pixels = vec![0; (w * h) as usize];
            s.windows.insert(
                id,
                Surface {
                    width: w,
                    height: h,
                    title,
                    front: Arc::new(pixels.clone()),
                    back: pixels,
                    sequence: 1,
                    input: InputState::default(),
                    closed: false,
                    flags: a[3] & 2,
                    resize: None,
                    activation: 0,
                    configuration: 0,
                },
            );
            a[0] = id;
            capture(&s)?;
            drop(s);
            self.host.request_hosted_window();
            return Ok(0);
        }
        let mut s = shared.lock().unwrap();
        let window = s.windows.get_mut(&a[0]).ok_or(22u16)?;
        match op {
            2 => {
                s.windows.remove(&a[0]);
            }
            3 | 5 | 6 => {
                let (x, y, w, h) = (a[1], a[2], a[3], a[4]);
                if x > window.width
                    || y > window.height
                    || w > window.width - x
                    || h > window.height - y
                {
                    return Err(22);
                }
                if w == 0 || h == 0 {
                    return Ok(0);
                }
                let source = if op == 3 {
                    if a[5] < w {
                        return Err(22);
                    }
                    let count = (h - 1)
                        .checked_mul(a[5])
                        .and_then(|n| n.checked_add(w))
                        .ok_or(14u16)?;
                    Some(span(a[6], count as usize)?)
                } else {
                    None
                };
                if op == 6 {
                    let (dx, dy) = (a[5], a[6]);
                    if dx > window.width
                        || dy > window.height
                        || w > window.width - dx
                        || h > window.height - dy
                    {
                        return Err(22);
                    }
                    // Snapshot the whole source rectangle for overlapping copies.
                    let mut copy = Vec::with_capacity((w * h) as usize);
                    for row in 0..h {
                        let at = ((y + row) * window.width + x) as usize;
                        copy.extend_from_slice(&window.back[at..at + w as usize]);
                    }
                    for row in 0..h {
                        let at = ((dy + row) * window.width + dx) as usize;
                        window.back[at..at + w as usize]
                            .copy_from_slice(&copy[(row * w) as usize..((row + 1) * w) as usize]);
                    }
                } else {
                    for row in 0..h {
                        for col in 0..w {
                            let colour = if let Some(start) = source {
                                self.memory.read(start + (row * a[5] + col) as usize)
                            } else {
                                a[5] as u16
                            };
                            window.back[((y + row) * window.width + x + col) as usize] = colour;
                        }
                    }
                }
            }
            4 => {
                window.front = Arc::new(window.back.clone());
                window.sequence = window.sequence.wrapping_add(1);
                if let Some(path) = std::env::var_os("FUSE_X_FRAME") {
                    let mut bytes =
                        format!("P6\n{} {}\n255\n", window.width, window.height).into_bytes();
                    for c in window.front.iter() {
                        bytes.extend(rvm::display_rgb565::rgb565_to_rgb888(*c));
                    }
                    std::fs::write(path, bytes).map_err(|_| 5u16)?;
                }
                if let Some(directory) = std::env::var_os("FUSE_X_FRAMES") {
                    let directory = std::path::Path::new(&directory);
                    std::fs::create_dir_all(directory).map_err(|_| 5u16)?;
                    let mut bytes =
                        format!("P6\n{} {}\n255\n", window.width, window.height).into_bytes();
                    for c in window.front.iter() {
                        bytes.extend(rvm::display_rgb565::rgb565_to_rgb888(*c));
                    }
                    let temporary = directory.join(format!("{}.ppm.tmp", a[0]));
                    std::fs::write(&temporary, bytes).map_err(|_| 5u16)?;
                    std::fs::rename(temporary, directory.join(format!("{}.ppm", a[0])))
                        .map_err(|_| 5u16)?;
                }
            }
            7 => {
                window.title = title(self, a[1])?;
            }
            8 => {
                // Validate before dequeueing: a bad destination cannot lose input.
                span(a[1], 21)?;
                let mut out = vec![1];
                pair(&mut out, a[0]);
                if window.input.overflow {
                    window.input.overflow = false;
                    // Overflow recovery may discard stale motion/keys, but a
                    // window-close request must still reach its owner.
                    window.input.events.retain(|e| e.kind == 8);
                    pair(&mut out, window.input.sequence);
                    out.extend([9, 0, 0, window.input.buttons]);
                    out.extend([0; 12]);
                } else if let Some((width, height)) = window.resize.take() {
                    pair(&mut out, window.input.sequence);
                    out.extend([10, 0, 0, 0]);
                    pair(&mut out, width);
                    pair(&mut out, height);
                    out.extend([0; 8]);
                } else if let Some(e) = window.input.events.pop_front() {
                    if e.kind == 8 {
                        window.closed = false;
                    }
                    pair(&mut out, e.sequence);
                    out.extend([e.kind, e.flags, e.code, e.buttons]);
                    for n in [e.x, e.y, e.dx, e.dy, e.wheel_x, e.wheel_y] {
                        pair(&mut out, n as u32);
                    }
                } else {
                    out.extend([0; 18]);
                }
                write_cells(self, a[1], &out)?;
            }
            9 => {
                let mut out = vec![1];
                pair(&mut out, a[0]);
                out.extend([window.input.flags(), window.input.buttons]);
                out.extend(window.input.held);
                pair(&mut out, window.input.x as u32);
                pair(&mut out, window.input.y as u32);
                write_cells(self, a[1], &out)?;
            }
            10 => {
                return Ok(u32::from(
                    window.input.overflow
                        || window.resize.is_some()
                        || !window.input.events.is_empty(),
                ))
            }
            12 => {
                let (width, height, flags) = (a[1], a[2], a[3]);
                if width == 0
                    || height == 0
                    || width > MAX_WIDTH
                    || height > MAX_HEIGHT
                    || flags & !3 != 0
                {
                    return Err(22);
                }
                if (width, height) != (window.width, window.height) {
                    let mut pixels = vec![0; (width * height) as usize];
                    for row in 0..height.min(window.height) {
                        let count = width.min(window.width) as usize;
                        pixels[(row * width) as usize..(row * width) as usize + count]
                            .copy_from_slice(
                                &window.back[(row * window.width) as usize
                                    ..(row * window.width) as usize + count],
                            );
                    }
                    window.width = width;
                    window.height = height;
                    window.back = pixels;
                    window.front = Arc::new(window.back.clone());
                    window.sequence = window.sequence.wrapping_add(1);
                }
                if window.resize == Some((width, height)) {
                    window.resize = None;
                }
                if window.flags & 2 == 0 && flags & 2 != 0 {
                    window.transition(InputTransition::Focus { focused: false });
                }
                window.flags = flags;
                window.configuration = window.configuration.wrapping_add(1);
            }
            13 => {
                window.activation = window.activation.wrapping_add(1);
            }
            _ => return Err(42),
        }
        if matches!(op, 2 | 4 | 7 | 12) {
            capture(&s)?;
        }
        Ok(0)
    }
}
#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_gx(ctx: *mut Runtime, op: u64, request: u64) -> u64 {
    let rt = unsafe { &mut *ctx };
    let result = (|| {
        let at = span(request as u32, 24)?;
        let mut a = [0; 12];
        for (i, n) in a.iter_mut().enumerate() {
            *n =
                rt.memory.read(at + 2 * i) as u32 | ((rt.memory.read(at + 2 * i + 1) as u32) << 16);
        }
        let result = rt.graphics_call(op, &mut a);
        for (i, n) in a.iter().enumerate() {
            rt.memory.write(at + 2 * i, *n as u16);
            rt.memory.write(at + 2 * i + 1, (*n >> 16) as u16);
        }
        result
    })();
    match result {
        Ok(n) => n as u64,
        Err(e) => (-(e as i32)) as u32 as u64,
    }
}

// All winit/Pixels objects stay on the Mac main thread. An empty registry is a
// live session: closing the last window never terminates the kernel/event loop.
pub(super) fn run(
    shared: Arc<Mutex<Shared>>,
) -> std::result::Result<(), Box<dyn std::error::Error>> {
    use pixels::{Pixels, SurfaceTexture};
    use winit::{
        dpi::LogicalSize,
        event::{ElementState, Event, MouseButton, MouseScrollDelta, WindowEvent},
        event_loop::{ControlFlow, EventLoopBuilder},
        platform::run_return::EventLoopExtRunReturn,
        window::WindowBuilder,
    };
    struct View {
        pixels: Pixels,
        window: winit::window::Window,
        sequence: u64,
        title: String,
        visible: bool,
        dimensions: (u32, u32),
        resizable: bool,
        activation: u64,
        configuration: u64,
    }
    let mut event_loop = EventLoopBuilder::new().build();
    #[cfg(target_os = "macos")]
    super::hosted_graphics_macos::independent_windows();
    let mut views: BTreeMap<u32, View> = BTreeMap::new();
    let mut failure = None;
    event_loop.run_return(|event, target, flow| {
        *flow = ControlFlow::WaitUntil(
            std::time::Instant::now() + std::time::Duration::from_millis(16),
        );
        if shared.lock().unwrap().finished {
            *flow = ControlFlow::Exit;
            return;
        }
        let result: std::result::Result<(), Box<dyn std::error::Error>> = (|| {
            match event {
                Event::MainEventsCleared => {
                    let descriptions: Vec<_> = {
                        let s = shared.lock().unwrap();
                        s.windows
                            .iter()
                            .filter(|(_, w)| w.flags & 2 == 0)
                            .map(|(&id, w)| {
                                (
                                    id,
                                    w.width,
                                    w.height,
                                    w.title.clone(),
                                    w.sequence,
                                    w.flags & 1 == 0,
                                    w.activation,
                                    w.configuration,
                                )
                            })
                            .collect()
                    };
                    views.retain(|id, _| descriptions.iter().any(|d| d.0 == *id));
                    for (
                        id,
                        width,
                        height,
                        title,
                        sequence,
                        resizable,
                        activation,
                        configuration,
                    ) in descriptions
                    {
                        if !views.contains_key(&id) {
                            let window = WindowBuilder::new()
                                .with_title(&title)
                                .with_inner_size(LogicalSize::new(width, height))
                                .with_resizable(resizable)
                                .build(target)?;
                            let size = window.inner_size();
                            let pixels = Pixels::new(
                                width,
                                height,
                                SurfaceTexture::new(size.width.max(1), size.height.max(1), &window),
                            )?;
                            views.insert(
                                id,
                                View {
                                    window,
                                    pixels,
                                    sequence: 0,
                                    title: title.clone(),
                                    visible: true,
                                    dimensions: (width, height),
                                    resizable,
                                    activation: 0,
                                    configuration,
                                },
                            );
                        }
                        let view = views.get_mut(&id).unwrap();
                        if view.activation != activation {
                            view.window.set_minimized(false);
                            view.window.focus_window();
                            view.activation = activation;
                        }
                        if view.dimensions != (width, height) {
                            view.pixels.resize_buffer(width, height)?;
                            view.dimensions = (width, height);
                            view.sequence = 0;
                        }
                        if view.configuration != configuration {
                            // A refused guest resize acknowledges the old size too.
                            // Restore native geometry even when pixels did not change.
                            view.window.set_inner_size(LogicalSize::new(width, height));
                            view.configuration = configuration;
                        }
                        if view.resizable != resizable {
                            view.window.set_resizable(resizable);
                            view.resizable = resizable;
                        }
                        if title != view.title {
                            view.window.set_title(&title);
                            view.title = title;
                        }
                        if sequence != view.sequence {
                            view.window.request_redraw();
                        }
                    }
                    #[cfg(target_os = "macos")]
                    {
                        use winit::platform::macos::WindowExtMacOS;
                        super::hosted_graphics_macos::update(
                            shared.clone(),
                            views
                                .iter()
                                .map(|(&id, v)| (id, v.title.clone(), v.window.ns_window()))
                                .collect(),
                        );
                    }
                }
                Event::RedrawRequested(window_id) => {
                    if let Some((&id, view)) =
                        views.iter_mut().find(|(_, v)| v.window.id() == window_id)
                    {
                        if !view.visible {
                            return Ok(());
                        }
                        let frame = {
                            let s = shared.lock().unwrap();
                            s.windows.get(&id).map(|w| (w.front.clone(), w.sequence))
                        };
                        if let Some((frame, sequence)) = frame {
                            if sequence != view.sequence {
                                for (pixel, c) in view
                                    .pixels
                                    .frame_mut()
                                    .chunks_exact_mut(4)
                                    .zip(frame.iter())
                                {
                                    let [r, g, b] = rvm::display_rgb565::rgb565_to_rgb888(*c);
                                    pixel.copy_from_slice(&[r, g, b, 255]);
                                }
                                view.sequence = sequence;
                            }
                            view.pixels.render()?;
                        }
                    }
                }
                Event::WindowEvent { window_id, event } => {
                    if let Some((&id, view)) =
                        views.iter_mut().find(|(_, v)| v.window.id() == window_id)
                    {
                        let mut s = shared.lock().unwrap();
                        let Some(w) = s.windows.get_mut(&id) else {
                            return Ok(());
                        };
                        match event {
                            WindowEvent::CloseRequested => w.transition(InputTransition::Close),
                            WindowEvent::Focused(focused) => {
                                w.transition(InputTransition::Focus { focused })
                            }
                            WindowEvent::CursorEntered { .. } => {
                                w.transition(InputTransition::Enter { inside: true })
                            }
                            WindowEvent::CursorLeft { .. } => {
                                w.transition(InputTransition::Enter { inside: false })
                            }
                            WindowEvent::KeyboardInput { input, .. } => {
                                #[cfg(target_os = "macos")]
                                {
                                    #[allow(deprecated)]
                                    let menu_shortcut = input.modifiers.logo()
                                        && matches!(
                                            input.virtual_keycode,
                                            Some(
                                                winit::event::VirtualKeyCode::N
                                                    | winit::event::VirtualKeyCode::Q
                                                    | winit::event::VirtualKeyCode::H
                                                    | winit::event::VirtualKeyCode::W
                                            )
                                        );
                                    if menu_shortcut {
                                        return Ok(());
                                    }
                                }
                                if let Some(key) = input
                                    .virtual_keycode
                                    .and_then(rvm::display_rgb565::key_usage)
                                {
                                    w.transition(InputTransition::Key {
                                        key,
                                        down: input.state == ElementState::Pressed,
                                    });
                                }
                            }
                            WindowEvent::ModifiersChanged(mods) => {
                                for (left, right, pressed) in [
                                    (224, 228, mods.ctrl()),
                                    (225, 229, mods.shift()),
                                    (226, 230, mods.alt()),
                                    (227, 231, mods.logo()),
                                ] {
                                    if !pressed {
                                        w.transition(InputTransition::Key {
                                            key: left,
                                            down: false,
                                        });
                                        w.transition(InputTransition::Key {
                                            key: right,
                                            down: false,
                                        });
                                    } else if !w.input.held(left) && !w.input.held(right) {
                                        w.transition(InputTransition::Key {
                                            key: left,
                                            down: true,
                                        });
                                    }
                                }
                            }
                            WindowEvent::CursorMoved { position, .. } => {
                                let (x, y) = match view
                                    .pixels
                                    .window_pos_to_pixel((position.x as f32, position.y as f32))
                                {
                                    Ok((x, y)) => (x as i32, y as i32),
                                    Err((x, y)) => (x as i32, y as i32),
                                };
                                w.transition(InputTransition::Motion { x, y });
                            }
                            WindowEvent::MouseInput { state, button, .. } => {
                                let button = match button {
                                    MouseButton::Left => 0,
                                    MouseButton::Right => 1,
                                    MouseButton::Middle => 2,
                                    MouseButton::Other(n) => n.saturating_add(3),
                                };
                                w.transition(InputTransition::Button {
                                    button,
                                    down: state == ElementState::Pressed,
                                    x: w.input.x,
                                    y: w.input.y,
                                });
                            }
                            WindowEvent::MouseWheel { delta, .. } => {
                                let (x, y) = match delta {
                                    MouseScrollDelta::LineDelta(x, y) => {
                                        (f64::from(x), f64::from(y))
                                    }
                                    MouseScrollDelta::PixelDelta(p) => (
                                        p.x / view.window.scale_factor() / 40.0,
                                        p.y / view.window.scale_factor() / 40.0,
                                    ),
                                };
                                w.transition(InputTransition::Wheel {
                                    x: (x * 65536.0).round() as i32,
                                    y: (y * 65536.0).round() as i32,
                                });
                            }
                            WindowEvent::Resized(size) => {
                                let logical = size.to_logical::<u32>(view.window.scale_factor());
                                w.request_resize(logical.width, logical.height);
                                drop(s);
                                view.visible = size.width > 0 && size.height > 0;
                                if view.visible {
                                    view.pixels.resize_surface(size.width, size.height)?;
                                    view.window.request_redraw();
                                }
                            }
                            WindowEvent::ScaleFactorChanged { new_inner_size, .. } => {
                                drop(s);
                                view.visible =
                                    new_inner_size.width > 0 && new_inner_size.height > 0;
                                if view.visible {
                                    view.pixels.resize_surface(
                                        new_inner_size.width,
                                        new_inner_size.height,
                                    )?;
                                    view.window.request_redraw();
                                }
                            }
                            _ => {}
                        }
                    }
                }
                _ => {}
            }
            Ok(())
        })();
        if let Err(e) = result {
            failure = Some(e);
            *flow = ControlFlow::Exit;
        }
    });
    if let Some(e) = failure {
        Err(e)
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn create(rt: &mut Runtime, w: u32, h: u32) -> u32 {
        let title = 0x10000;
        rt.memory.write(BANK, 0);
        let mut a = [0; 12];
        a[0] = w;
        a[1] = h;
        a[2] = title;
        rt.graphics_call(1, &mut a).unwrap();
        a[0]
    }
    #[test]
    fn surfaces_copy_overlap_publish_snapshots_and_reject_stale_handles() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 3, 2);
        let second = create(&mut rt, 2, 2);
        let ptr = 0x20000;
        for i in 0..6 {
            rt.memory.write(2 * BANK + i, (i + 1) as u16);
        }
        let mut a = [id, 0, 0, 3, 2, 3, ptr, 0, 0, 0, 0, 0];
        rt.graphics_call(3, &mut a).unwrap();
        a = [id, 0, 0, 2, 2, 1, 0, 0, 0, 0, 0, 0];
        rt.graphics_call(6, &mut a).unwrap();
        a = [0; 12];
        a[0] = id;
        rt.graphics_call(4, &mut a).unwrap();
        let snapshot = rt.graphics.shared.lock().unwrap().windows[&id]
            .front
            .clone();
        assert_eq!(&**snapshot, &[1, 1, 2, 4, 4, 5]);
        a = [id, 0, 0, 3, 2, 9, 0, 0, 0, 0, 0, 0];
        rt.graphics_call(5, &mut a).unwrap();
        assert_eq!(&**snapshot, &[1, 1, 2, 4, 4, 5]);
        assert_eq!(
            rt.graphics.shared.lock().unwrap().windows[&second].back,
            vec![0; 4]
        );
        a = [id, 3, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0];
        assert_eq!(rt.graphics_call(5, &mut a), Err(22));
        a = [0; 12];
        a[0] = id;
        rt.graphics_call(2, &mut a).unwrap();
        assert_eq!(rt.graphics_call(4, &mut a), Err(22));
        assert!(create(&mut rt, 1, 1) > second);
    }
    #[test]
    fn input_is_per_window_focus_releases_keys_and_close_keeps_session_alive() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 3, 2);
        let other = create(&mut rt, 3, 2);
        {
            let mut s = rt.graphics.shared.lock().unwrap();
            let w = s.windows.get_mut(&id).unwrap();
            w.transition(InputTransition::Focus { focused: true });
            w.transition(InputTransition::Key { key: 4, down: true });
            w.transition(InputTransition::Close);
            assert!(!w.input.held(4));
            assert!(!s.finished);
        }
        let mut a = [0; 12];
        a[0] = id;
        assert_eq!(rt.graphics_call(10, &mut a), Ok(1));
        a[0] = other;
        assert_eq!(rt.graphics_call(10, &mut a), Ok(0));
        a[0] = id;
        a[1] = 0;
        assert_eq!(rt.graphics_call(8, &mut a), Err(14));
        let mut closed = false;
        a[1] = 0x30000;
        while rt.graphics_call(10, &mut a) == Ok(1) {
            rt.graphics_call(8, &mut a).unwrap();
            closed |= rt.memory.read(3 * BANK + 5) == 8;
        }
        assert!(closed);
        rt.graphics_call(9, &mut a).unwrap();
        assert_eq!(rt.memory.read(3 * BANK + 3) & 8, 0);
    }
    #[test]
    fn resource_and_pointer_limits_are_bounded() {
        let mut rt = Runtime::new();
        let mut a = [1025, 1, 0x10000, 0, 0, 0, 0, 0, 0, 0, 0, 0];
        assert_eq!(rt.graphics_call(1, &mut a), Err(22));
        for _ in 0..MAX_WINDOWS {
            create(&mut rt, 1, 1);
        }
        a[0] = 1;
        assert_eq!(rt.graphics_call(1, &mut a), Err(12));
        assert_eq!(span(0x1fa00, 1), Err(14));
        assert_eq!(span(0xfffffa00, 1), Err(14));
    }
    #[test]
    fn overflow_preserves_close_and_an_application_can_decline_then_close_again() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 3, 2);
        {
            let mut s = rt.graphics.shared.lock().unwrap();
            let w = s.windows.get_mut(&id).unwrap();
            for x in 0..300 {
                w.transition(InputTransition::Motion { x, y: 0 });
            }
            w.transition(InputTransition::Close);
        }
        let mut a = [0; 12];
        a[0] = id;
        a[1] = 0x30000;
        rt.graphics_call(8, &mut a).unwrap();
        assert_eq!(rt.memory.read(3 * BANK + 5), 9);
        rt.graphics_call(8, &mut a).unwrap();
        assert_eq!(rt.memory.read(3 * BANK + 5), 8);
        rt.graphics
            .shared
            .lock()
            .unwrap()
            .windows
            .get_mut(&id)
            .unwrap()
            .transition(InputTransition::Close);
        rt.graphics_call(8, &mut a).unwrap();
        assert_eq!(rt.memory.read(3 * BANK + 5), 8);
    }
    #[test]
    fn resize_requests_coalesce_per_window_and_owner_acknowledges_geometry() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 3, 2);
        let other = create(&mut rt, 2, 2);
        let mut a = [id, 0, 0, 3, 2, 0x1234, 0, 0, 0, 0, 0, 0];
        rt.graphics_call(5, &mut a).unwrap();
        {
            let mut s = rt.graphics.shared.lock().unwrap();
            let w = s.windows.get_mut(&id).unwrap();
            w.request_resize(7, 5);
            w.request_resize(12, 9);
        }
        a = [0; 12];
        a[0] = other;
        assert_eq!(rt.graphics_call(10, &mut a), Ok(0));
        a[0] = id;
        assert_eq!(rt.graphics_call(10, &mut a), Ok(1));
        a[1] = 0x30000;
        rt.graphics_call(8, &mut a).unwrap();
        assert_eq!(rt.memory.read(3 * BANK + 5), 10);
        assert_eq!(rt.memory.read(3 * BANK + 9), 12);
        assert_eq!(rt.memory.read(3 * BANK + 11), 9);
        a = [id, 10, 8, 2, 0, 0, 0, 0, 0, 0, 0, 0];
        rt.graphics_call(12, &mut a).unwrap();
        {
            let s = rt.graphics.shared.lock().unwrap();
            let w = &s.windows[&id];
            assert_eq!((w.width, w.height, w.flags), (10, 8, 2));
            assert_eq!(&w.front[..4], &[0x1234, 0x1234, 0x1234, 0]);
            assert_eq!(s.windows[&other].width, 2);
            assert_eq!(w.configuration, 1);
        }
        // A resize refusal acknowledges the previous size, without new pixels.
        let sequence = rt.graphics.shared.lock().unwrap().windows[&id].sequence;
        rt.graphics_call(12, &mut a).unwrap();
        {
            let s = rt.graphics.shared.lock().unwrap();
            assert_eq!(s.windows[&id].configuration, 2);
            assert_eq!(s.windows[&id].sequence, sequence);
        }
        let mut activate = [0; 12];
        activate[0] = other;
        rt.graphics_call(13, &mut activate).unwrap();
        assert_eq!(rt.graphics.shared.lock().unwrap().windows[&other].activation, 1);
        a[3] = 4;
        assert_eq!(rt.graphics_call(12, &mut a), Err(22));
        a[3] = 1;
        rt.graphics_call(12, &mut a).unwrap();
        rt.graphics
            .shared
            .lock()
            .unwrap()
            .windows
            .get_mut(&id)
            .unwrap()
            .request_resize(20, 20);
        assert_eq!(rt.graphics_call(10, &mut a), Ok(0));
    }
}
