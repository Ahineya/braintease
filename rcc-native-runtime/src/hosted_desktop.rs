//! Optional nested GX desktop. Policy/composition is portable; this adapter
//! owns snapshots, guest input delivery, and the single winit/Pixels presenter.
use super::*;
#[path = "../../libraries/graphics/desktop.rs"]
pub(super) mod policy;
use policy::{Action, Control, Desktop, Rect, TASKBAR};
#[path = "../../libraries/graphics/desktop_text.rs"]
mod text;

pub(super) fn enabled() -> bool {
    std::env::var("FUSE_X_DESKTOP").as_deref() == Ok("1")
}
#[derive(Clone)]
struct Description {
    width: u32,
    height: u32,
    sequence: u64,
    configuration: u64,
    activation: u64,
    title: String,
    scale: u32,
}
struct Frame {
    id: u32,
    width: u32,
    height: u32,
    rgba: Option<Arc<Vec<u32>>>,
    rgb565: Arc<Vec<u16>>,
}
pub(super) struct Session {
    pub(super) wm: Desktop,
    pub(super) scale: u32,
    frames: BTreeMap<u32, Description>,
    pixels: Vec<u32>,
    hover: Option<u32>,
    host_focused: bool,
    activate_host: bool,
    presents: u64,
    composed_pixels: u64,
    last_damage: Vec<Rect>,
}
impl Default for Session {
    fn default() -> Self {
        let size = std::env::var("FUSE_X_DESKTOP_SIZE")
            .ok()
            .and_then(|s| {
                s.split_once('x')
                    .and_then(|(w, h)| Some((w.parse().ok()?, h.parse().ok()?)))
            })
            .unwrap_or((1280, 800));
        Self {
            wm: Desktop::new(size.0, size.1),
            scale: 1,
            frames: BTreeMap::new(),
            pixels: Vec::new(),
            hover: None,
            host_focused: true,
            activate_host: false,
            presents: 0,
            composed_pixels: 0,
            last_damage: Vec::new(),
        }
    }
}
impl Session {
    fn apply(&mut self, s: &mut Shared, actions: policy::Actions) {
        for a in actions.iter() {
            match a {
                Action::Focus(id, focused) => {
                    if let Some(w) = s.windows.get_mut(&id) {
                        w.transition(InputTransition::Focus {
                            focused: focused && self.host_focused,
                        });
                    }
                }
                Action::Resize(id, width, height) => {
                    if let Some(w) = s.windows.get_mut(&id) {
                        w.request_resize(width, height);
                    }
                }
                Action::Close(id) => {
                    if let Some(w) = s.windows.get_mut(&id) {
                        w.transition(InputTransition::Close);
                    }
                }
                Action::Capture(id, captured) => {
                    if let Some(w) = s.windows.get_mut(&id) {
                        w.transition(InputTransition::Capture { captured });
                    }
                }
                Action::Motion(id, x, y) => {
                    if let Some(w) = s.windows.get_mut(&id) {
                        w.transition(InputTransition::Motion { x, y });
                    }
                }
                Action::Button(id, button, down, x, y) => {
                    if let Some(w) = s.windows.get_mut(&id) {
                        w.transition(InputTransition::Button { button, down, x, y });
                    }
                }
            }
        }
    }
    fn sync(&mut self, s: &mut Shared) {
        let removed: Vec<_> = self
            .wm
            .windows()
            .iter()
            .filter(|w| s.windows.get(&w.id).is_none_or(|w| w.flags & 2 != 0))
            .map(|w| w.id)
            .collect();
        for id in removed {
            let a = self.wm.remove(id);
            self.apply(s, a);
            self.frames.remove(&id);
        }
        let descriptions: Vec<_> = s
            .windows
            .iter()
            .filter(|(_, w)| w.flags & 2 == 0)
            .map(|(&id, w)| {
                (
                    id,
                    w.width,
                    w.height,
                    w.flags,
                    w.sequence,
                    w.configuration,
                    w.activation,
                    w.title.clone(),
                    w.pixel_scale(),
                )
            })
            .collect();
        for (id, width, height, flags, sequence, configuration, activation, title, scale) in
            descriptions
        {
            if let Some(w) = s.windows.get_mut(&id) {
                w.prefer_scale(self.scale);
            }
            if self.wm.get(id).is_none() {
                self.activate_host = true;
                let a = self.wm.add(id, width, height, flags & 1 == 0);
                self.apply(s, a);
            } else if let Some(old) = self.frames.get(&id).cloned() {
                if (old.width, old.height, old.configuration) != (width, height, configuration) {
                    self.wm.accepted(id, width, height, flags & 1 == 0);
                }
                if old.activation != activation {
                    self.activate_host = true;
                    let a = self.wm.activate(id);
                    self.apply(s, a);
                }
                if old.sequence != sequence || old.title != title || old.scale != scale {
                    let win = self.wm.get(id).unwrap();
                    if !win.minimized {
                        self.wm.invalidate(win.outer());
                    }
                    if old.title != title {
                        self.wm.invalidate(Rect::new(
                            0,
                            self.wm.height - TASKBAR,
                            self.wm.width,
                            TASKBAR,
                        ));
                    }
                }
            }
            if let Some(w) = s.windows.get(&id) {
                if w.resize.is_none() && w.resize_in_flight.is_none() {
                    if let Some(win) = self.wm.get(id) {
                        if (win.content.w, win.content.h) != (width as i32, height as i32) {
                            self.wm.accepted(id, width, height, flags & 1 == 0);
                        }
                    }
                }
            }
            self.frames.insert(
                id,
                Description {
                    width,
                    height,
                    sequence,
                    configuration,
                    activation,
                    title,
                    scale,
                },
            );
        }
        // Host pointer queries use workspace coordinates translated per client,
        // even when outside a client's visible region or during title dragging.
        for w in self.wm.windows() {
            if let Some(client) = s.windows.get_mut(&w.id) {
                if client.pointer_requested {
                    client.desktop_pointer = Some((
                        self.wm.pointer.0.saturating_sub(w.content.x),
                        self.wm.pointer.1.saturating_sub(w.content.y),
                    ));
                }
            }
        }
        // A close request may be declined by an unsaved-document dialog.
        // Once delivered, restore keyboard focus to the still-live client.
        if self.host_focused {
            if let Some(w) = self.wm.focused.and_then(|id| s.windows.get_mut(&id)) {
                if !w.closed && !w.input.focused {
                    w.transition(InputTransition::Focus { focused: true });
                }
            }
        }
        self.update_hover(s);
    }
    fn update_hover(&mut self, s: &mut Shared) {
        let hit = self.wm.hit_client(self.wm.pointer.0, self.wm.pointer.1);
        if hit != self.hover {
            if let Some(id) = self.hover {
                if let Some(w) = s.windows.get_mut(&id) {
                    w.transition(InputTransition::Enter { inside: false });
                }
            }
            if let Some(id) = hit {
                if let Some(w) = s.windows.get_mut(&id) {
                    w.transition(InputTransition::Enter { inside: true });
                }
            }
            self.hover = hit;
        }
    }
    pub(super) fn resize(&mut self, s: &mut Shared, w: u32, h: u32) {
        let actions = self.wm.host_resize(w, h);
        self.apply(s, actions);
    }
    pub(super) fn density(&mut self, s: &mut Shared, scale: u32) {
        if self.scale != scale.clamp(1, 2) {
            self.scale = scale.clamp(1, 2);
            self.pixels.clear();
            self.wm
                .invalidate(Rect::new(0, 0, self.wm.width, self.wm.height));
            for w in s.windows.values_mut() {
                w.prefer_scale(self.scale);
            }
        }
    }
    pub(super) fn input(&mut self, s: &mut Shared, input: InputTransition) {
        if !input.valid() {
            return;
        }
        match input {
            InputTransition::Motion { x, y } => {
                let a = self.wm.motion(x, y);
                self.apply(s, a);
                self.update_hover(s);
            }
            InputTransition::Button { button, down, x, y } => {
                let a = self.wm.motion(x, y);
                self.apply(s, a);
                let a = self.wm.button(button, down);
                self.apply(s, a);
                self.update_hover(s);
            }
            InputTransition::Wheel { .. } => {
                if let Some(id) = self.wm.hit_client(self.wm.pointer.0, self.wm.pointer.1) {
                    if let Some(w) = s.windows.get_mut(&id) {
                        w.transition(input);
                    }
                }
            }
            InputTransition::Focus { focused } => {
                self.host_focused = focused;
                if focused {
                    if let Some(id) = self.wm.focused {
                        if let Some(w) = s.windows.get_mut(&id) {
                            w.transition(input);
                        }
                    }
                } else {
                    let a = self.wm.lose_focus();
                    self.apply(s, a);
                }
            }
            InputTransition::Enter { inside: false } => {
                if let Some(id) = self.hover.take() {
                    if let Some(w) = s.windows.get_mut(&id) {
                        w.transition(InputTransition::Enter { inside: false });
                    }
                }
            }
            InputTransition::Enter { .. } | InputTransition::Capture { .. } => {}
            _ => {
                if let Some(id) = self.wm.focused {
                    if let Some(w) = s.windows.get_mut(&id) {
                        w.transition(input);
                    }
                }
            }
        }
    }
    fn snapshots(&self, s: &Shared) -> Vec<Frame> {
        self.wm
            .windows()
            .iter()
            .filter(|w| !w.minimized)
            .filter_map(|w| {
                s.windows.get(&w.id).map(|f| Frame {
                    id: w.id,
                    width: f.width * f.pixel_scale(),
                    height: f.height * f.pixel_scale(),
                    rgba: f.rgba(),
                    rgb565: f.front.clone(),
                })
            })
            .collect()
    }
    fn compose(&mut self, frames: &[Frame]) -> bool {
        let width = self.wm.width as u32 * self.scale;
        let height = self.wm.height as u32 * self.scale;
        if self.pixels.len() != width as usize * height as usize {
            self.pixels = vec![0; width as usize * height as usize];
            self.wm
                .invalidate(Rect::new(0, 0, self.wm.width, self.wm.height));
        }
        let (damage, count) = self.wm.take_damage();
        if count == 0 {
            return false;
        }
        self.last_damage = damage[..count].to_vec();
        for &d in &damage[..count] {
            let full = Rect::new(0, 0, self.wm.width, self.wm.height);
            policy::fill(
                &mut self.pixels,
                width,
                height,
                self.scale,
                full,
                d,
                colour::rgba(0x15191d),
            );
            for win in self.wm.windows().iter().filter(|w| !w.minimized) {
                let focused = self.wm.focused == Some(win.id);
                let outer = win.outer();
                let chrome = win.chrome();
                policy::fill(
                    &mut self.pixels,
                    width,
                    height,
                    self.scale,
                    outer,
                    d,
                    colour::rgba(if focused { 0x565f6d } else { 0x343a42 }),
                );
                policy::fill(
                    &mut self.pixels,
                    width,
                    height,
                    self.scale,
                    chrome.title,
                    d,
                    colour::rgba(if focused { 0x2b3038 } else { 0x22262d }),
                );
                if let Some(frame) = frames.iter().find(|f| f.id == win.id) {
                    let source = match &frame.rgba {
                        Some(p) => policy::Pixels::Rgba(p),
                        None => policy::Pixels::Rgb565(&frame.rgb565),
                    };
                    policy::blit(
                        &mut self.pixels,
                        width,
                        height,
                        self.scale,
                        win.content,
                        d,
                        source,
                        frame.width,
                        frame.height,
                    );
                }
                if let Some(meta) = self.frames.get(&win.id) {
                    text::paint(
                        &mut self.pixels,
                        width,
                        height,
                        self.scale,
                        chrome.label,
                        d,
                        &meta.title,
                        colour::rgba(if focused { 0xd8dce3 } else { 0xa1a8b3 }),
                        false,
                    );
                }
                for (kind, r, enabled) in [
                    (Control::Close, chrome.close, true),
                    (Control::Minimize, chrome.minimize, true),
                    (Control::Maximize, chrome.maximize, win.resizable),
                ] {
                    control_icon(
                        &mut self.pixels,
                        width,
                        height,
                        self.scale,
                        r,
                        d,
                        kind,
                        enabled,
                        self.wm.hovered_control() == Some((win.id, kind)),
                        win.maximized,
                        focused,
                    );
                }
                if win.resizable && !win.maximized {
                    for n in 0..3 {
                        let x = win.content.x + win.content.w - 5 - n * 4;
                        let y = win.content.y + win.content.h - 3;
                        policy::fill(
                            &mut self.pixels,
                            width,
                            height,
                            self.scale,
                            Rect::new(x, y - n * 4, 2, 2),
                            d,
                            colour::rgba(0x89939f),
                        );
                    }
                }
            }
            policy::fill(
                &mut self.pixels,
                width,
                height,
                self.scale,
                Rect::new(0, self.wm.height - TASKBAR, self.wm.width, TASKBAR),
                d,
                colour::rgba(0x20252b),
            );
            for (i, w) in self.wm.windows().iter().enumerate() {
                let r = self.wm.task_rect(i);
                policy::fill(
                    &mut self.pixels,
                    width,
                    height,
                    self.scale,
                    r,
                    d,
                    colour::rgba(if self.wm.focused == Some(w.id) {
                        0x394653
                    } else {
                        0x2b3139
                    }),
                );
                if let Some(meta) = self.frames.get(&w.id) {
                    label(
                        &mut self.pixels,
                        width,
                        height,
                        self.scale,
                        r.x + 8,
                        r.y + (r.h - 17) / 2,
                        r.w - 16,
                        &meta.title,
                        d,
                        colour::rgba(if w.minimized { 0x8893a1 } else { 0xd8dce3 }),
                    );
                }
            }
            self.composed_pixels += d.w as u64 * d.h as u64 * self.scale as u64 * self.scale as u64;
        }
        self.presents += 1;
        true
    }
    fn metadata(&self) -> serde_json::Value {
        let rect = |r: Rect| [r.x, r.y, r.w, r.h];
        let windows:Vec<_>=self.wm.windows().iter().enumerate().map(|(z,w)|{
            let source=self.frames.get(&w.id);let c=w.chrome();
            serde_json::json!({"id":w.id,"x":w.content.x,"y":w.content.y,"width":w.content.w,"height":w.content.h,"minimized":w.minimized,"maximized":w.maximized,"normal_bounds":rect(w.normal),"focused":self.wm.focused==Some(w.id),"z":z,"resizable":w.resizable,"title":source.map(|s|s.title.as_str()),"sequence":source.map(|s|s.sequence),"source_scale":source.map(|s|s.scale),"taskbar":rect(self.wm.task_rect(z)),"chrome":{"title":rect(c.title),"label":rect(c.label),"drag":rect(c.drag),"grip":rect(c.grip),"close":rect(c.close),"minimize":rect(c.minimize),"maximize":rect(c.maximize),"maximize_enabled":w.resizable}})
        }).collect();
        let damage: Vec<_> = self
            .last_damage
            .iter()
            .map(|r| [r.x, r.y, r.w, r.h])
            .collect();
        serde_json::json!({"width":self.wm.width,"height":self.wm.height,"scale":self.scale,"pixel_width":self.wm.width as u32*self.scale,"pixel_height":self.wm.height as u32*self.scale,"focused":self.wm.focused,"maximize_bounds":rect(self.wm.maximized_bounds()),"windows":windows,"presents":self.presents,"composed_pixels":self.composed_pixels,"damage":damage,"native_windows":1})
    }
    fn write_capture(&self) -> Result<()> {
        let Some(directory) = std::env::var_os("FUSE_X_FRAMES") else {
            return Ok(());
        };
        let directory = std::path::Path::new(&directory);
        std::fs::create_dir_all(directory).map_err(|_| 5u16)?;
        let mut bytes = format!(
            "P6\n{} {}\n255\n",
            self.wm.width as u32 * self.scale,
            self.wm.height as u32 * self.scale
        )
        .into_bytes();
        bytes.reserve(self.pixels.len() * 3);
        for c in &self.pixels {
            bytes.extend_from_slice(&c.to_le_bytes()[..3]);
        }
        std::fs::write(directory.join("desktop.ppm.tmp"), bytes).map_err(|_| 5u16)?;
        std::fs::rename(
            directory.join("desktop.ppm.tmp"),
            directory.join("desktop.ppm"),
        )
        .map_err(|_| 5u16)?;
        std::fs::write(
            directory.join("desktop.json.tmp"),
            serde_json::to_vec(&self.metadata()).unwrap(),
        )
        .map_err(|_| 5u16)?;
        std::fs::rename(
            directory.join("desktop.json.tmp"),
            directory.join("desktop.json"),
        )
        .map_err(|_| 5u16)?;
        Ok(())
    }
}

// Flat monochrome controls use the same portable hit rectangles as input.
fn control_icon(
    out: &mut [u32],
    width: u32,
    height: u32,
    scale: u32,
    r: Rect,
    damage: Rect,
    kind: Control,
    enabled: bool,
    hovered: bool,
    maximized: bool,
    focused: bool,
) {
    if r.empty() {
        return;
    }
    if hovered && enabled {
        policy::fill(out, width, height, scale, r, damage, colour::rgba(0x3c434d));
    }
    let colour = colour::rgba(if !enabled {
        0x525861
    } else if focused {
        0xd8dce3
    } else {
        0x929ba8
    });
    let cx = r.x + r.w / 2;
    let cy = r.y + r.h / 2;
    let mut fill =
        |rect: Rect| policy::fill(out, width, height, scale, rect, r.intersect(damage), colour);
    let mut outline = |x: i32, y: i32, w: i32, h: i32| {
        fill(Rect::new(x, y, w, 1));
        fill(Rect::new(x, y + h - 1, w, 1));
        fill(Rect::new(x, y, 1, h));
        fill(Rect::new(x + w - 1, y, 1, h));
    };
    match kind {
        Control::Close => {
            for n in 0..9 {
                fill(Rect::new(cx - 4 + n, cy - 4 + n, 1, 1));
                fill(Rect::new(cx + 4 - n, cy - 4 + n, 1, 1));
            }
        }
        Control::Minimize => fill(Rect::new(cx - 4, cy, 9, 1)),
        Control::Maximize => {
            if maximized {
                outline(cx - 2, cy - 4, 7, 6);
                outline(cx - 5, cy - 1, 7, 6);
            } else {
                outline(cx - 4, cy - 4, 9, 8);
            }
        }
    }
}
fn label(
    out: &mut [u32],
    width: u32,
    height: u32,
    scale: u32,
    x: i32,
    y: i32,
    max_width: i32,
    text: &str,
    damage: Rect,
    c: u32,
) {
    text::paint(
        out,
        width,
        height,
        scale,
        Rect::new(x, y, max_width, 17),
        damage,
        text,
        c,
        false,
    );
}

pub(super) fn headless_capture(s: &mut Shared) -> Result<()> {
    let mut session = s.desktop.take().unwrap_or_default();
    session.sync(s);
    let frames = session.snapshots(s);
    let changed = session.compose(&frames);
    let result = if changed {
        session.write_capture()
    } else {
        Ok(())
    };
    s.desktop = Some(session);
    result
}
pub(super) fn headless_event(s: &mut Shared, command: &serde_json::Value) -> Result<()> {
    let mut session = s.desktop.take().unwrap_or_default();
    session.sync(s);
    let result = (|| {
        if let Some(input) = command.get("input") {
            let t: InputTransition = serde_json::from_value(input.clone()).map_err(|_| 22u16)?;
            if !t.valid() {
                return Err(22);
            }
            session.input(s, t);
        } else if let Some(size) = command.get("resize") {
            let a = size.as_array().filter(|a| a.len() == 2).ok_or(22u16)?;
            let w = a[0]
                .as_u64()
                .filter(|&n| (320..=1920).contains(&n))
                .ok_or(22u16)?;
            let h = a[1]
                .as_u64()
                .filter(|&n| (240..=1080).contains(&n))
                .ok_or(22u16)?;
            session.resize(s, w as u32, h as u32);
        } else if let Some(scale) = command.get("density") {
            let scale = scale
                .as_u64()
                .filter(|&n| (1..=2).contains(&n))
                .ok_or(22u16)?;
            session.density(s, scale as u32);
        } else if let Some(action) = command.get("action") {
            let command = action
                .as_u64()
                .filter(|&n| (1..=4).contains(&n))
                .ok_or(22u16)?;
            session.input(
                s,
                InputTransition::Edit {
                    command: command as u16,
                },
            );
        } else if command.get("close").and_then(|v| v.as_bool()) == Some(true) {
            session.input(s, InputTransition::Close);
        } else {
            return Err(22);
        }
        let frames = session.snapshots(s);
        if session.compose(&frames) {
            session.write_capture()?;
        }
        Ok(())
    })();
    s.desktop = Some(session);
    result
}

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
    let mut event_loop = EventLoopBuilder::new().build();
    #[cfg(target_os = "macos")]
    super::super::hosted_graphics_macos::independent_windows();
    let mut session = Session::default();
    let window = WindowBuilder::new()
        .with_title("Fuse-X Desktop")
        .with_inner_size(LogicalSize::new(session.wm.width, session.wm.height))
        .with_min_inner_size(LogicalSize::new(320, 240))
        .with_max_inner_size(LogicalSize::new(1920, 1080))
        .build(&event_loop)?;
    window.set_ime_allowed(true);
    let size = window.inner_size();
    session.density(
        &mut shared.lock().unwrap(),
        if window.scale_factor() >= 1.5 { 2 } else { 1 },
    );
    let mut pixels = Pixels::new(
        session.wm.width as u32 * session.scale,
        session.wm.height as u32 * session.scale,
        SurfaceTexture::new(size.width.max(1), size.height.max(1), &window),
    )?;
    let renderer = PresentationRenderer::new(pixels.device(), pixels.surface_texture_format());
    let mut visible = true;
    let mut dirty = true;
    let mut failure = None;
    let mut pointer = (0, 0);
    event_loop.run_return(|event, _, flow| {
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
                    let frames = {
                        let mut s = shared.lock().unwrap();
                        session.sync(&mut s);
                        session.snapshots(&s)
                    };
                    if session.activate_host {
                        session.activate_host = false;
                        window.set_minimized(false);
                        window.focus_window();
                    }
                    if session.compose(&frames) {
                        dirty = true;
                        session
                            .write_capture()
                            .map_err(|e| format!("desktop capture: {e}"))?;
                    }
                    if dirty && visible {
                        window.request_redraw();
                    }
                    #[cfg(target_os = "macos")]
                    {
                        use winit::platform::macos::WindowExtMacOS;
                        super::super::hosted_graphics_macos::update(
                            shared.clone(),
                            vec![(
                                session.wm.focused.unwrap_or(0),
                                "Fuse-X Desktop".into(),
                                window.ns_window(),
                            )],
                        );
                    }
                }
                Event::RedrawRequested(id) if id == window.id() => {
                    if visible {
                        let w = session.wm.width as u32 * session.scale;
                        let h = session.wm.height as u32 * session.scale;
                        if pixels.context().texture_extent.width != w
                            || pixels.context().texture_extent.height != h
                        {
                            pixels.resize_buffer(w, h)?;
                            dirty = true;
                        }
                        if session.pixels.len() == (w * h) as usize {
                            if dirty {
                                for (p, c) in
                                    pixels.frame_mut().chunks_exact_mut(4).zip(&session.pixels)
                                {
                                    p.copy_from_slice(&c.to_le_bytes());
                                }
                            }
                            let size = window.inner_size();
                            pixels.render_with(|encoder, target, context| {
                                renderer.render(
                                    encoder,
                                    target,
                                    context,
                                    (size.width, size.height),
                                );
                                Ok(())
                            })?;
                            dirty = false;
                        }
                    }
                }
                Event::WindowEvent { window_id, event } if window_id == window.id() => {
                    let mut s = shared.lock().unwrap();
                    match event {
                        WindowEvent::CloseRequested => {
                            session.input(&mut s, InputTransition::Focus { focused: false });
                            window.set_minimized(true);
                        }
                        WindowEvent::Focused(focused) => {
                            session.input(&mut s, InputTransition::Focus { focused })
                        }
                        WindowEvent::CursorLeft { .. } => {
                            session.input(&mut s, InputTransition::Enter { inside: false })
                        }
                        WindowEvent::CursorMoved { position, .. } => {
                            let size = window.inner_size();
                            let (x, y) = presentation_pixel(
                                (position.x as f32, position.y as f32),
                                (
                                    session.wm.width as u32 * session.scale,
                                    session.wm.height as u32 * session.scale,
                                ),
                                (size.width.max(1), size.height.max(1)),
                            );
                            pointer = (
                                x.div_euclid(session.scale as i32),
                                y.div_euclid(session.scale as i32),
                            );
                            session.input(
                                &mut s,
                                InputTransition::Motion {
                                    x: pointer.0,
                                    y: pointer.1,
                                },
                            );
                        }
                        WindowEvent::MouseInput { state, button, .. } => {
                            let button = match button {
                                MouseButton::Left => 0,
                                MouseButton::Right => 1,
                                MouseButton::Middle => 2,
                                MouseButton::Other(n) => n.saturating_add(3),
                            };
                            session.input(
                                &mut s,
                                InputTransition::Button {
                                    button,
                                    down: state == ElementState::Pressed,
                                    x: pointer.0,
                                    y: pointer.1,
                                },
                            );
                        }
                        WindowEvent::MouseWheel { delta, .. } => {
                            let (x, y) = match delta {
                                MouseScrollDelta::LineDelta(x, y) => (x as f64, y as f64),
                                MouseScrollDelta::PixelDelta(p) => (
                                    p.x / window.scale_factor() / 40.0,
                                    p.y / window.scale_factor() / 40.0,
                                ),
                            };
                            session.input(
                                &mut s,
                                InputTransition::Wheel {
                                    x: (x * 65536.0).round() as i32,
                                    y: (y * 65536.0).round() as i32,
                                },
                            );
                        }
                        WindowEvent::KeyboardInput { input, .. } => {
                            #[allow(deprecated)]
                            let menu = input.modifiers.logo()
                                && matches!(
                                    input.virtual_keycode,
                                    Some(
                                        winit::event::VirtualKeyCode::N
                                            | winit::event::VirtualKeyCode::Q
                                            | winit::event::VirtualKeyCode::H
                                            | winit::event::VirtualKeyCode::W
                                            | winit::event::VirtualKeyCode::C
                                            | winit::event::VirtualKeyCode::V
                                            | winit::event::VirtualKeyCode::X
                                            | winit::event::VirtualKeyCode::A
                                    )
                                );
                            if !menu {
                                if let Some(key) = input
                                    .virtual_keycode
                                    .and_then(rvm::display_rgb565::key_usage)
                                {
                                    #[allow(deprecated)]
                                    let text = rvm::display_rgb565::text_usage(key)
                                        && !input.modifiers.ctrl()
                                        && !input.modifiers.logo();
                                    let down = input.state == ElementState::Pressed;
                                    session.input(
                                        &mut s,
                                        if text {
                                            InputTransition::TextKey { key, down }
                                        } else {
                                            InputTransition::Key { key, down }
                                        },
                                    );
                                }
                            }
                        }
                        WindowEvent::ReceivedCharacter(ch) => {
                            let held = session
                                .wm
                                .focused
                                .and_then(|id| s.windows.get(&id))
                                .is_some_and(|w| {
                                    [224, 228, 227, 231].iter().any(|&key| w.input.held(key))
                                });
                            if !ch.is_control() && !held {
                                session.input(&mut s, InputTransition::Text { scalar: ch as u32 });
                            }
                        }
                        WindowEvent::Ime(winit::event::Ime::Commit(text)) => {
                            let held = session
                                .wm
                                .focused
                                .and_then(|id| s.windows.get(&id))
                                .is_some_and(|w| {
                                    [224, 228, 227, 231].iter().any(|&key| w.input.held(key))
                                });
                            if !held {
                                for ch in text.chars().filter(|ch| !ch.is_control()) {
                                    session
                                        .input(&mut s, InputTransition::Text { scalar: ch as u32 });
                                }
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
                                    session.input(
                                        &mut s,
                                        InputTransition::Key {
                                            key: left,
                                            down: false,
                                        },
                                    );
                                    session.input(
                                        &mut s,
                                        InputTransition::Key {
                                            key: right,
                                            down: false,
                                        },
                                    );
                                } else {
                                    let held = session
                                        .wm
                                        .focused
                                        .and_then(|id| s.windows.get(&id))
                                        .is_some_and(|w| w.input.held(left) || w.input.held(right));
                                    if !held {
                                        session.input(
                                            &mut s,
                                            InputTransition::Key {
                                                key: left,
                                                down: true,
                                            },
                                        );
                                    }
                                }
                            }
                        }
                        WindowEvent::Resized(size) => {
                            visible = size.width > 0 && size.height > 0;
                            if visible {
                                let logical = size.to_logical::<u32>(window.scale_factor());
                                session.resize(&mut s, logical.width, logical.height);
                                pixels.resize_surface(size.width, size.height)?;
                                dirty = true;
                            }
                        }
                        WindowEvent::ScaleFactorChanged {
                            scale_factor,
                            new_inner_size,
                        } => {
                            session.density(&mut s, if scale_factor >= 1.5 { 2 } else { 1 });
                            visible = new_inner_size.width > 0 && new_inner_size.height > 0;
                            if visible {
                                let logical = new_inner_size.to_logical::<u32>(scale_factor);
                                session.resize(&mut s, logical.width, logical.height);
                                pixels
                                    .resize_surface(new_inner_size.width, new_inner_size.height)?;
                                dirty = true;
                            }
                        }
                        _ => {}
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
    match failure {
        Some(e) => Err(e),
        None => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn create(rt: &mut Runtime, width: u32, height: u32, format: u32) -> u32 {
        rt.memory.write(BANK, 0);
        let mut args = [0; 12];
        args[0] = width;
        args[1] = height;
        args[2] = 0x10000;
        args[4] = format;
        rt.graphics_call(1, &mut args).unwrap();
        args[0]
    }
    fn paint(s: &mut Shared, id: u32, c: u32) {
        let w = s.windows.get_mut(&id).unwrap();
        let f = w.colour.as_mut().unwrap();
        f.back.fill(c);
        f.present();
        w.sequence += 1;
    }
    #[test]
    fn session_idle_and_published_frame_isolation() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 80, 60, 2);
        let mut s = rt.graphics.shared.lock().unwrap();
        paint(&mut s, id, colour::rgba(0x217ca1));
        let original = s.windows[&id].colour.as_ref().unwrap().front.clone();
        s.windows
            .get_mut(&id)
            .unwrap()
            .colour
            .as_mut()
            .unwrap()
            .back
            .fill(colour::rgba(0xd43545));
        let mut session = Session::default();
        session.sync(&mut s);
        assert!(session.compose(&session.snapshots(&s)));
        let r = session.wm.get(id).unwrap().content;
        assert_eq!(
            session.pixels[(r.y * session.wm.width + r.x) as usize],
            colour::rgba(0x217ca1)
        );
        assert!(original.iter().all(|&p| p == colour::rgba(0x217ca1)));
        let presents = session.presents;
        session.sync(&mut s);
        assert!(!session.compose(&session.snapshots(&s)));
        assert_eq!(session.presents, presents);
        paint(&mut s, id, colour::rgba(0xf1ac21));
        session.sync(&mut s);
        assert!(session.compose(&session.snapshots(&s)));
        assert_eq!(
            session.pixels[(r.y * session.wm.width + r.x) as usize],
            colour::rgba(0xf1ac21)
        );
        assert!(original.iter().all(|&p| p == colour::rgba(0x217ca1)));
    }
    #[test]
    fn input_focus_capture_coordinates_and_owner_cleanup() {
        let mut rt = Runtime::new();
        let first = create(&mut rt, 160, 120, 2);
        let second = create(&mut rt, 160, 120, 2);
        let mut s = rt.graphics.shared.lock().unwrap();
        let mut session = Session::default();
        session.sync(&mut s);
        let r = session.wm.get(first).unwrap().content;
        session.input(
            &mut s,
            InputTransition::Button {
                button: 0,
                down: true,
                x: r.x + 3,
                y: r.y + 3,
            },
        );
        assert_eq!(session.wm.focused, Some(first));
        assert!(s.windows[&first].input.captured);
        session.input(&mut s, InputTransition::Motion { x: -50, y: -40 });
        assert_eq!(s.windows[&first].input.x, -50 - r.x);
        assert_eq!(s.windows[&first].input.y, -40 - r.y);
        session.input(&mut s, InputTransition::Key { key: 4, down: true });
        assert!(s.windows[&first].input.held(4));
        assert!(!s.windows[&second].input.held(4));
        session.input(&mut s, InputTransition::Focus { focused: false });
        assert!(!s.windows[&first].input.captured);
        assert_eq!(s.windows[&first].input.buttons, 0);
        assert!(!s.windows[&first].input.held(4));
        session.input(&mut s, InputTransition::Focus { focused: true });
        session.input(&mut s, InputTransition::Close);
        assert!(s.windows[&first].closed);
        assert!(!s.windows[&first].input.focused);
        s.windows.get_mut(&first).unwrap().closed = false; // owner consumed and declined close
        session.sync(&mut s);
        assert!(s.windows[&first].input.focused);
        s.windows.remove(&first);
        session.sync(&mut s);
        assert_eq!(session.wm.focused, Some(second));
        assert!(session.wm.get(first).is_none());
        session.input(&mut s, InputTransition::Focus { focused: true });
        assert!(s.windows[&second].input.focused);
        s.windows.get_mut(&second).unwrap().flags |= 2;
        session.sync(&mut s);
        assert!(session.wm.windows().is_empty());
        assert_eq!(session.wm.focused, None);
    }
    #[test]
    fn incremental_move_matches_complete_composition() {
        let mut rt = Runtime::new();
        let first = create(&mut rt, 200, 140, 2);
        let second = create(&mut rt, 220, 160, 2);
        let mut s = rt.graphics.shared.lock().unwrap();
        paint(&mut s, first, colour::rgba(0xff5500));
        paint(&mut s, second, colour::rgba(0x11aa44));
        let mut session = Session::default();
        session.density(&mut s, 2);
        session.sync(&mut s);
        session.compose(&session.snapshots(&s));
        let r = session.wm.get(second).unwrap().chrome().drag;
        session.input(
            &mut s,
            InputTransition::Button {
                button: 0,
                down: true,
                x: r.x + 10,
                y: r.y + r.h / 2,
            },
        );
        session.input(
            &mut s,
            InputTransition::Motion {
                x: r.x + 160,
                y: r.y + r.h / 2 + 90,
            },
        );
        session.input(
            &mut s,
            InputTransition::Button {
                button: 0,
                down: false,
                x: r.x + 160,
                y: r.y + r.h / 2 + 90,
            },
        );
        assert!(session.compose(&session.snapshots(&s)));
        let incremental = session.pixels.clone();
        session
            .wm
            .invalidate(Rect::new(0, 0, session.wm.width, session.wm.height));
        session.compose(&session.snapshots(&s));
        assert_eq!(incremental, session.pixels);
    }
    #[test]
    fn density_and_resize_are_bounded_and_independent_of_client_pixels() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 80, 60, 2);
        let mut s = rt.graphics.shared.lock().unwrap();
        s.windows.get_mut(&id).unwrap().automatic_scale = true;
        let original = s.windows[&id].colour.as_ref().unwrap().front.clone();
        let mut session = Session::default();
        session.sync(&mut s);
        session.density(&mut s, 2);
        assert_eq!(s.windows[&id].preferred_scale, 2);
        assert_eq!(s.windows[&id].resize, Some((80, 60)));
        session.resize(&mut s, 1920, 1080);
        session.compose(&session.snapshots(&s));
        assert_eq!(session.pixels.len(), 3840 * 2160);
        assert_eq!(original.len(), 80 * 60);
        session.resize(&mut s, 320, 240);
        session.density(&mut s, 1);
        session.compose(&session.snapshots(&s));
        assert_eq!(session.pixels.len(), 320 * 240);
    }
    fn click_control(session: &mut Session, s: &mut Shared, id: u32, kind: Control) {
        let c = session.wm.get(id).unwrap().chrome();
        let r = match kind {
            Control::Close => c.close,
            Control::Minimize => c.minimize,
            Control::Maximize => c.maximize,
        };
        for down in [true, false] {
            session.input(
                s,
                InputTransition::Button {
                    button: 0,
                    down,
                    x: r.x + r.w / 2,
                    y: r.y + r.h / 2,
                },
            );
        }
    }
    #[test]
    fn maximize_workspace_resize_restore_and_close_veto_route_real_host_requests() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 300, 360, 2);
        let mut s = rt.graphics.shared.lock().unwrap();
        let mut session = Session::default();
        session.sync(&mut s);
        let normal = session.wm.get(id).unwrap().content;
        click_control(&mut session, &mut s, id, Control::Maximize);
        assert_eq!(s.windows[&id].resize, Some((1276, 736)));
        assert!(session.wm.get(id).unwrap().maximized);
        session.resize(&mut s, 1470, 900);
        assert_eq!(s.windows[&id].resize, Some((1466, 836)));
        assert_eq!(session.wm.get(id).unwrap().normal, normal);
        click_control(&mut session, &mut s, id, Control::Close);
        assert!(s.windows[&id].closed);
        assert!(session.wm.get(id).unwrap().maximized);
        s.windows.get_mut(&id).unwrap().closed = false;
        session.sync(&mut s);
        assert!(s.windows[&id].input.focused);
        assert_eq!(session.wm.get(id).unwrap().normal, normal);
        click_control(&mut session, &mut s, id, Control::Maximize);
        assert_eq!(s.windows[&id].resize, Some((300, 360)));
        assert_eq!(session.wm.get(id).unwrap().content, normal);
    }
    #[test]
    fn fixed_client_maximize_is_disabled_in_metadata_and_paint_and_invalid_buttons_are_ignored() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 300, 200, 2);
        let mut s = rt.graphics.shared.lock().unwrap();
        s.windows.get_mut(&id).unwrap().flags = 1;
        let mut session = Session::default();
        session.sync(&mut s);
        let before = session.wm.get(id).unwrap().content;
        click_control(&mut session, &mut s, id, Control::Maximize);
        assert!(s.windows[&id].resize.is_none());
        assert_eq!(session.wm.get(id).unwrap().content, before);
        assert!(!session.wm.get(id).unwrap().maximized);
        assert_eq!(
            session.metadata()["windows"][0]["chrome"]["maximize_enabled"],
            false
        );
        session.compose(&session.snapshots(&s));
        let c = session.wm.get(id).unwrap().chrome().maximize;
        let x = c.x + c.w / 2 - 4;
        let y = c.y + c.h / 2 - 4;
        assert_eq!(
            session.pixels[(y * session.wm.width + x) as usize],
            colour::rgba(0x525861)
        );
        let pointer = session.wm.pointer;
        session.input(
            &mut s,
            InputTransition::Button {
                button: 16,
                down: true,
                x: i32::MIN,
                y: i32::MAX,
            },
        );
        assert_eq!(session.wm.pointer, pointer);
        assert_eq!(session.wm.get(id).unwrap().content, before);
    }
}
