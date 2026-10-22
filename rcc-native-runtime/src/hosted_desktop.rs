//! Optional nested GX desktop. Policy/composition is portable; this adapter
//! owns snapshots, guest input delivery, and the single winit/Pixels presenter.
use super::*;
#[path = "../../libraries/graphics/desktop.rs"]
pub(super) mod policy;
use policy::{Action, Control, Desktop, Rect};
#[path = "../../libraries/graphics/dock.rs"]
mod dock;
#[path = "../../libraries/graphics/desktop_dock.rs"]
mod dock_view;
#[path = "../../libraries/graphics/desktop_icons.rs"]
mod icons;
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
#[derive(Clone)]
struct Overlay {
    panel: panel::Panel,
    sequence: u64,
    configuration: u64,
    width: u32,
    height: u32,
    scale: u32,
    alpha: Option<Arc<Vec<u32>>>,
}
impl PartialEq for Overlay {
    fn eq(&self, other: &Self) -> bool {
        self.panel == other.panel && self.sequence == other.sequence && self.configuration == other.configuration
            && self.width == other.width && self.height == other.height && self.scale == other.scale
    }
}
impl Eq for Overlay {}
pub(super) struct Session {
    pub(super) wm: Desktop,
    pub(super) scale: u32,
    dock: dock_view::View,
    overlay: Option<Overlay>,
    panel_buttons: u16,
    panel_keys: [bool; 256],
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
        let mut wm = Desktop::new(size.0, size.1);
        wm.reserve_bottom(dock_view::HEIGHT);
        wm.enable_taskbar_input(false);
        let dock = dock_view::View::new(wm.width, wm.height);
        Self {
            wm,
            dock,
            overlay: None,
            panel_buttons: 0,
            panel_keys: [false; 256],
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
        let panel_keyboard = self.panel_keyboard();
        for a in actions.iter() {
            match a {
                Action::Focus(id, focused) => {
                    if let Some(w) = s.windows.get_mut(&id) {
                        w.transition(InputTransition::Focus {
                            focused: focused
                                && self.host_focused
                                && !self.dock.modal()
                                && panel_keyboard.is_none(),
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
    pub(super) fn sync(&mut self, s: &mut Shared) {
        self.sync_overlay(s);
        let panel_window = s.panel.as_ref().map(|p| p.window);
        let removed: Vec<_> = self
            .wm
            .windows()
            .iter()
            .filter(|w| {
                panel_window == Some(w.id) || s.windows.get(&w.id).is_none_or(|w| w.flags & 2 != 0)
            })
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
            .filter(|(id, w)| panel_window != Some(**id) && w.flags & 2 == 0)
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
                            self.wm.height - dock_view::HEIGHT,
                            self.wm.width,
                            dock_view::HEIGHT,
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
        self.sync_dock(s);
        self.update_hover(s);
    }
    fn panel_keyboard(&self) -> Option<u32> {
        self.overlay
            .as_ref()
            .filter(|o| o.panel.flags & panel::KEYBOARD != 0)
            .map(|o| o.panel.window)
    }
    fn panel_hit(&self, x: i32, y: i32) -> Option<u32> {
        self.overlay
            .as_ref()
            .filter(|o| {
                self.panel_buttons != 0
                    || o.panel.flags & panel::MODAL != 0
                    || (o.panel.regions.iter().any(|r| r.contains(x, y))
                        && (o.panel.flags & panel::ALPHA == 0 || o.alpha.as_ref().is_some_and(|pixels|
                            policy::alpha_hit(pixels,o.width*o.scale,o.height*o.scale,o.width,o.height,x,y))))
            })
            .map(|o| o.panel.window)
    }
    fn sync_overlay(&mut self, s: &mut Shared) {
        let old = self.overlay.clone();
        let next = s.panel.as_ref().and_then(|panel| {
            let surface = s.windows.get(&panel.window)?;
            if surface.flags & 2 != 0 || panel.regions.is_empty() {
                return None;
            }
            Some(Overlay {
                panel: panel.clone(),
                sequence: surface.sequence,
                configuration: surface.configuration,
                width: surface.width,
                height: surface.height,
                scale: surface.pixel_scale(),
                alpha: if panel.flags & panel::ALPHA != 0 { surface.rgba() } else { None },
            })
        });
        if old != next {
            for overlay in old.iter().chain(next.iter()) {
                for &r in &overlay.panel.regions {
                    self.wm.invalidate(r);
                }
            }
            if old.is_some() != next.is_some() {
                self.wm.invalidate(self.dock.bounds());
                self.dock.lost_focus();
            }
        }
        if old.as_ref().map(|o| o.panel.window) != next.as_ref().map(|o| o.panel.window) {
            if let Some(id) = old.as_ref().map(|o| o.panel.window) {
                if let Some(window) = s.windows.get_mut(&id) {
                    window.transition(InputTransition::Capture { captured: false });
                }
            }
            self.panel_buttons = 0;
            self.panel_keys.fill(false);
        }
        let was_keyboard = self.panel_keyboard().is_some();
        self.overlay = next;
        if !was_keyboard && self.panel_keyboard().is_some() {
            let actions = self.wm.lose_focus();
            self.apply(s, actions);
        }
        let (top, bottom) = self.overlay.as_ref().map_or((0, dock_view::HEIGHT), |o| {
            (o.panel.reserve_top, o.panel.reserve_bottom)
        });
        let actions = self.wm.reserve_edges(top, bottom);
        self.apply(s, actions);
        if let Some(overlay) = &self.overlay {
            if let Some(surface) = s.windows.get_mut(&overlay.panel.window) {
                surface.prefer_scale(self.scale);
            }
        }
    }
    fn sync_dock(&mut self, s: &mut Shared) {
        let records: Vec<_> = self
            .wm
            .windows()
            .iter()
            .filter_map(|window| {
                let surface = s.windows.get(&window.id)?;
                let process = s.registry.process(surface.process);
                let instance = process.and_then(|p| s.registry.instance(p.instance));
                Some(dock::WindowRecord {
                    id: window.id,
                    process: surface.process,
                    instance: instance.map_or(0, |i| i.id),
                    app_id: instance.map_or("", |i| i.app_id.as_str()),
                    name: instance.map_or("", |i| i.name.as_str()),
                    icon: instance.map_or("", |i| i.icon.as_str()),
                    title: &surface.title,
                    role: 0,
                    top_level: true,
                    flags: if window.minimized {
                        dock::MINIMIZED
                    } else {
                        dock::VISIBLE
                    } | if self.wm.focused == Some(window.id) {
                        dock::FOCUSED
                    } else {
                        0
                    } | if surface.closed {
                        dock::CLOSE_PENDING
                    } else {
                        0
                    },
                })
            })
            .collect();
        let before = self.dock.bounds();
        if self
            .dock
            .synchronize(&records, self.wm.width, self.wm.height)
        {
            self.wm.invalidate(before);
            self.wm.invalidate(self.dock.bounds());
        }
        // Menu modality belongs to the desktop, including when windows appear
        // or disappear without another input event. Focus transitions also
        // release held client keys before menu navigation can consume them.
        let target = self.panel_keyboard().or(self.wm.focused);
        for (&id, window) in &mut s.windows {
            let focused =
                target == Some(id) && self.host_focused && !self.dock.modal() && !window.closed;
            if window.input.focused != focused {
                window.transition(InputTransition::Focus { focused });
            }
        }
    }
    fn update_hover(&mut self, s: &mut Shared) {
        let hit = if self.wm.pointer_captured() {
            self.wm.hit_client(self.wm.pointer.0, self.wm.pointer.1)
        } else if let Some(id) = self.panel_hit(self.wm.pointer.0, self.wm.pointer.1) {
            Some(id)
        } else if self.overlay.is_none()
            && (self.dock.modal() || self.dock.covers(self.wm.pointer.0, self.wm.pointer.1))
        {
            None
        } else {
            self.wm.hit_client(self.wm.pointer.0, self.wm.pointer.1)
        };
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
        if let Some(panel) = &s.panel {
            if let Some(surface) = s.windows.get_mut(&panel.window) {
                surface.request_resize(self.wm.width as u32, self.wm.height as u32);
            }
        }
        self.sync_overlay(s);
        self.sync_dock(s);
    }
    /// Apply validated desktop service requests through the mouse policy path.
    pub(super) fn command(
        &mut self,
        s: &mut Shared,
        id: u32,
        command: policy::WindowCommand,
    ) -> Result<()> {
        let actions = self.wm.command(id, command).map_err(|error| match error {
            policy::CommandError::NotFound => 22u16,
            policy::CommandError::Unsupported => 44u16,
            policy::CommandError::InvalidGeometry => 22u16,
        })?;
        self.apply(s, actions);
        if matches!(
            command,
            policy::WindowCommand::Activate
                | policy::WindowCommand::Restore
                | policy::WindowCommand::Maximize
        ) {
            self.activate_host = true;
        }
        self.sync_dock(s);
        self.update_hover(s);
        Ok(())
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
        // The shell is an independent client. Only its declared regions
        // intercept the pointer, except while a shell menu is explicitly modal.
        // An existing ordinary-client capture keeps its paired release.
        if let InputTransition::Focus { focused: false } = input {
            self.panel_buttons = 0;
            self.panel_keys.fill(false);
            if let Some(o) = &self.overlay {
                if let Some(w) = s.windows.get_mut(&o.panel.window) {
                    w.transition(InputTransition::Capture { captured: false });
                }
            }
        }
        // A menu owns the complete key sequence it accepted, even if its
        // press dismissed the menu before release/text reaches the adapter.
        let key_target = match input {
            InputTransition::Key { key, down } | InputTransition::TextKey { key, down } => {
                let owned = self.panel_keys[key as usize];
                let target = if owned {
                    self.overlay.as_ref().map(|o| o.panel.window)
                } else {
                    self.panel_keyboard()
                };
                if target.is_some() {
                    self.panel_keys[key as usize] = down;
                }
                target
            }
            InputTransition::Text { .. } | InputTransition::Edit { .. }
                if self.panel_keys.iter().any(|&v| v) =>
            {
                self.overlay.as_ref().map(|o| o.panel.window)
            }
            _ => None,
        };
        if let Some(id) = key_target {
            if let Some(w) = s.windows.get_mut(&id) {
                w.transition(input);
            }
            return;
        }
        if !self.wm.pointer_captured() {
            let point = match input {
                InputTransition::Motion { x, y } | InputTransition::Button { x, y, .. } => {
                    Some((x, y))
                }
                _ => None,
            };
            if let Some((x, y)) = point {
                if let Some(id) = self.panel_hit(x, y) {
                    self.wm.point(x, y);
                    if let Some(w) = s.windows.get_mut(&id) {
                        w.transition(InputTransition::Motion { x, y });
                        if let InputTransition::Button { button, down, .. } = input {
                            let was = self.panel_buttons;
                            if down {
                                self.panel_buttons |= 1 << button;
                            } else {
                                self.panel_buttons &= !(1 << button);
                            }
                            if was == 0 && self.panel_buttons != 0 {
                                w.transition(InputTransition::Capture { captured: true });
                            }
                            w.transition(input);
                            if was != 0 && self.panel_buttons == 0 {
                                w.transition(InputTransition::Capture { captured: false });
                            }
                        }
                    }
                    self.update_hover(s);
                    self.sync_dock(s);
                    return;
                }
            }
            if let InputTransition::Wheel { .. } = input {
                if let Some(id) = self.panel_hit(self.wm.pointer.0, self.wm.pointer.1) {
                    if let Some(w) = s.windows.get_mut(&id) {
                        w.transition(input);
                    }
                    return;
                }
            }
        }
        let before = self.dock.bounds();
        let mut interaction = dock_view::Interaction::default();
        let mut changed = false;
        match input {
            _ if self.overlay.is_some() => {}
            InputTransition::Motion { x, y } if !self.wm.pointer_captured() => {
                changed = self.dock.motion(x, y);
                if self.dock.modal() || self.dock.covers(x, y) {
                    self.wm.point(x, y);
                    interaction.consumed = true;
                }
            }
            InputTransition::Button { button, down, x, y }
                if !self.wm.pointer_captured() || self.dock.owns_button(button) =>
            {
                changed = self.dock.motion(x, y);
                interaction = self.dock.button(button, down, x, y);
                if interaction.consumed {
                    self.wm.point(x, y);
                    changed = true;
                }
            }
            InputTransition::Wheel { x, y } if !self.wm.pointer_captured() => {
                interaction.consumed = self.dock.wheel(
                    self.wm.pointer.0,
                    self.wm.pointer.1,
                    if y != 0 { y } else { -x },
                );
                changed = interaction.consumed;
            }
            InputTransition::Key { key, down } | InputTransition::TextKey { key, down } => {
                interaction = self.dock.key(key, down);
                changed = interaction.consumed;
            }
            InputTransition::Text { .. } | InputTransition::Edit { .. }
                if self.dock.suppress_text() =>
            {
                interaction.consumed = true;
            }
            InputTransition::Focus { focused: false } => {
                self.dock.lost_focus();
                changed = true;
            }
            _ => {}
        }
        if changed {
            self.wm.invalidate(before);
            self.wm.invalidate(self.dock.bounds());
        }
        for (id, command) in interaction.requests {
            if let Ok(actions) = self.wm.command(id, command) {
                self.apply(s, actions);
            }
        }
        if interaction.consumed {
            self.sync_dock(s);
            self.update_hover(s);
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
                    if let Some(id) = self.panel_keyboard().or(self.wm.focused) {
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
                if let Some(id) = self.panel_keyboard().or(self.wm.focused) {
                    if let Some(w) = s.windows.get_mut(&id) {
                        w.transition(input);
                    }
                }
            }
        }
        self.sync_dock(s);
    }
    fn snapshots(&self, s: &Shared) -> Vec<Frame> {
        let mut frames: Vec<_> = self
            .wm
            .windows()
            .iter()
            .filter(|w| !w.minimized)
            .filter_map(|w| {
                let f = s.windows.get(&w.id)?;
                Some(Frame {
                    id: w.id,
                    width: f.width * f.pixel_scale(),
                    height: f.height * f.pixel_scale(),
                    rgba: f.rgba(),
                    rgb565: f.front.clone(),
                })
            })
            .collect();
        if let Some(overlay) = &self.overlay {
            if let Some(f) = s.windows.get(&overlay.panel.window) {
                frames.push(Frame {
                    id: overlay.panel.window,
                    width: f.width * f.pixel_scale(),
                    height: f.height * f.pixel_scale(),
                    rgba: f.rgba(),
                    rgb565: f.front.clone(),
                });
            }
        }
        frames
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
                let shell_hover = self
                    .panel_hit(self.wm.pointer.0, self.wm.pointer.1)
                    .is_some();
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
                        !self.dock.modal()
                            && !shell_hover
                            && self.wm.hovered_control() == Some((win.id, kind)),
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
            if let Some(overlay) = &self.overlay {
                if let Some(frame) = frames.iter().find(|f| f.id == overlay.panel.window) {
                    if overlay.panel.flags & panel::ALPHA != 0 {
                        if let Some(source) = &frame.rgba {
                            policy::blit_alpha(&mut self.pixels,width,height,self.scale,
                                Rect::new(0,0,overlay.width as i32,overlay.height as i32),d,
                                source,frame.width,frame.height,&overlay.panel.regions);
                        }
                    } else { for &region in &overlay.panel.regions {
                        let source = match &frame.rgba {
                            Some(p) => policy::Pixels::Rgba(p),
                            None => policy::Pixels::Rgb565(&frame.rgb565),
                        };
                        policy::blit(
                            &mut self.pixels,
                            width,
                            height,
                            self.scale,
                            Rect::new(0, 0, overlay.width as i32, overlay.height as i32),
                            d.intersect(region),
                            source,
                            frame.width,
                            frame.height,
                        );
                    } }
                }
            } else {
                self.dock
                    .paint(&mut self.pixels, width, height, self.scale, d);
            }
            self.composed_pixels += d.w as u64 * d.h as u64 * self.scale as u64 * self.scale as u64;
        }
        self.presents += 1;
        true
    }
    pub(super) fn metadata(&self) -> serde_json::Value {
        let rect = |r: Rect| [r.x, r.y, r.w, r.h];
        let windows:Vec<_>=self.wm.windows().iter().enumerate().map(|(z,w)|{
            let source=self.frames.get(&w.id);let c=w.chrome();
            serde_json::json!({"id":w.id,"x":w.content.x,"y":w.content.y,"width":w.content.w,"height":w.content.h,"minimized":w.minimized,"maximized":w.maximized,"normal_bounds":rect(w.normal),"focused":self.wm.focused==Some(w.id),"z":z,"resizable":w.resizable,"title":source.map(|s|s.title.as_str()),"sequence":source.map(|s|s.sequence),"source_scale":source.map(|s|s.scale),"taskbar":rect(self.dock.window_rect(w.id)),"chrome":{"title":rect(c.title),"label":rect(c.label),"drag":rect(c.drag),"grip":rect(c.grip),"close":rect(c.close),"minimize":rect(c.minimize),"maximize":rect(c.maximize),"maximize_enabled":w.resizable}})
        }).collect();
        let damage: Vec<_> = self
            .last_damage
            .iter()
            .map(|r| [r.x, r.y, r.w, r.h])
            .collect();
        let key = |key: &dock::GroupKey| match key {
            dock::GroupKey::Application(id) => format!("app:{id}"),
            dock::GroupKey::Process(id) => format!("process:{id}"),
            dock::GroupKey::Window(id) => format!("window:{id}"),
        };
        let groups:Vec<_>=self.dock.model.groups().iter().map(|g|serde_json::json!({
            "key":key(&g.key),"app_id":match &g.key{dock::GroupKey::Application(id)=>id.as_str(),_=>""},
            "name":g.name,"icon":g.icon,"windows":g.windows.iter().map(|w|w.id).collect::<Vec<_>>(),
            "rect":rect(self.dock.group_rect(&g.key)),"focused":g.focused(),"minimized":g.all_minimized(),
        })).collect();
        let menu=self.dock.menu.as_ref().filter(|_|self.dock.modal()).map(|group|serde_json::json!({
            "group":key(group),"rect":rect(self.dock.menu_rect()),"scroll":self.dock.scroll,
            "entries":self.dock.entries().iter().map(|e|serde_json::json!({
                "window":e.window,"command":e.command,"title":e.title,"rect":rect(e.rect),"disabled":e.disabled,
            })).collect::<Vec<_>>(),
        }));
        let dock = serde_json::json!({"visible":self.overlay.is_none(),"bounds":rect(self.dock.bar()),"surface":rect(self.dock.footprint()),"groups":groups,"menu":menu,"offset":self.dock.offset});
        let panel=self.overlay.as_ref().map(|o|serde_json::json!({"window":o.panel.window,"keyboard":o.panel.flags&panel::KEYBOARD!=0,"modal":o.panel.flags&panel::MODAL!=0,"alpha":o.panel.flags&panel::ALPHA!=0,"regions":o.panel.regions.iter().map(|r|rect(*r)).collect::<Vec<_>>(),"reserve_top":o.panel.reserve_top,"reserve_bottom":o.panel.reserve_bottom,"sequence":o.sequence}));
        serde_json::json!({"panel":panel,"dock":dock,"width":self.wm.width,"height":self.wm.height,"scale":self.scale,"pixel_width":self.wm.width as u32*self.scale,"pixel_height":self.wm.height as u32*self.scale,"focused":self.wm.focused,"maximize_bounds":rect(self.wm.maximized_bounds()),"windows":windows,"presents":self.presents,"composed_pixels":self.composed_pixels,"damage":damage,"native_windows":1})
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
/// Panel discovery shares the exact policy/output used by the presenter.
pub(super) fn output_dimensions(s: &mut Shared) -> (u16, u16) {
    let session = s.desktop.get_or_insert_with(Session::default);
    (session.wm.width as u16, session.wm.height as u16)
}
pub(super) fn panel_changed(s: &mut Shared) {
    let mut session = s.desktop.take().unwrap_or_default();
    session.sync(s);
    s.desktop = Some(session);
}
pub(super) fn headless_capture(s: &mut Shared) -> Result<()> {
    let mut session = s.desktop.take().unwrap_or_default();
    session.sync(s);
    s.desktop = Some(session);
    headless_capture_synced(s)
}
/// Present an already synchronized policy without changing geometry twice.
pub(super) fn headless_capture_synced(s: &mut Shared) -> Result<()> {
    let mut session = s.desktop.take().unwrap_or_default();
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
    let (desktop_width, desktop_height) = {
        let mut s = shared.lock().unwrap();
        let session = s.desktop.get_or_insert_with(Session::default);
        (session.wm.width, session.wm.height)
    };
    let window = WindowBuilder::new()
        .with_title("Fuse-X Desktop")
        .with_inner_size(LogicalSize::new(desktop_width, desktop_height))
        .with_min_inner_size(LogicalSize::new(320, 240))
        .with_max_inner_size(LogicalSize::new(1920, 1080))
        .build(&event_loop)?;
    window.set_ime_allowed(true);
    let size = window.inner_size();
    let (pixel_width, pixel_height) = {
        let mut s = shared.lock().unwrap();
        let mut session = s.desktop.take().unwrap_or_default();
        session.density(&mut s, if window.scale_factor() >= 1.5 { 2 } else { 1 });
        let dimensions = (
            session.wm.width as u32 * session.scale,
            session.wm.height as u32 * session.scale,
        );
        s.desktop = Some(session);
        dimensions
    };
    let mut pixels = Pixels::new(
        pixel_width,
        pixel_height,
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
        let mut s = shared.lock().unwrap();
        if s.finished {
            *flow = ControlFlow::Exit;
            return;
        }
        let mut session = s.desktop.take().unwrap_or_default();
        #[cfg(target_os = "macos")]
        let mut mac_windows = None;
        let result: std::result::Result<(), Box<dyn std::error::Error>> = (|| {
            match event {
                Event::MainEventsCleared => {
                    session.sync(&mut s);
                    let frames = session.snapshots(&s);
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
                        mac_windows = Some(vec![(
                            session.wm.focused.unwrap_or(0),
                            "Fuse-X Desktop".into(),
                            window.ns_window(),
                        )]);
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
        s.desktop = Some(session);
        drop(s);
        // Cocoa clipboard integration locks Shared itself; publish only after
        // restoring the one policy instance and releasing its mutex.
        #[cfg(target_os = "macos")]
        if let Some(windows) = mac_windows {
            super::super::hosted_graphics_macos::update(shared.clone(), windows);
        }
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
        assert_eq!(s.windows[&id].resize, Some((1276, 706)));
        assert!(session.wm.get(id).unwrap().maximized);
        session.resize(&mut s, 1470, 900);
        assert_eq!(s.windows[&id].resize, Some((1466, 806)));
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
    #[test]
    fn dock_menu_keeps_new_and_activated_clients_unfocused_then_repairs_on_anchor_exit() {
        let mut rt = Runtime::new();
        let anchor = create(&mut rt, 200, 120, 2);
        let other = create(&mut rt, 200, 120, 2);
        let mut s = rt.graphics.shared.lock().unwrap();
        for (id, process, app) in [(anchor, 100, "anchor"), (other, 200, "other")] {
            s.windows.get_mut(&id).unwrap().process = process;
            s.registry
                .register(process, process as u16, app, app, "terminal")
                .unwrap();
        }
        let mut session = Session::default();
        session.sync(&mut s);
        let r = session.dock.window_rect(anchor);
        for down in [true, false] {
            session.input(
                &mut s,
                InputTransition::Button {
                    button: 1,
                    down,
                    x: r.x + r.w / 2,
                    y: r.y + r.h / 2,
                },
            );
        }
        assert!(session.dock.modal());
        assert!(s.windows.values().all(|w| !w.input.focused));
        session
            .command(&mut s, other, policy::WindowCommand::Activate)
            .unwrap();
        assert!(!s.windows[&other].input.focused);
        drop(s);
        let newcomer = create(&mut rt, 200, 120, 2);
        let mut s = rt.graphics.shared.lock().unwrap();
        session.sync(&mut s);
        assert_eq!(session.wm.focused, Some(newcomer));
        assert!(s.windows.values().all(|w| !w.input.focused));
        s.windows.remove(&anchor);
        session.sync(&mut s);
        assert!(!session.dock.modal());
        assert!(s.windows[&newcomer].input.focused);
    }
    #[test]
    fn client_capture_owns_release_over_dock_without_opening_its_menu() {
        let mut rt = Runtime::new();
        let id = create(&mut rt, 200, 120, 2);
        let mut s = rt.graphics.shared.lock().unwrap();
        let mut session = Session::default();
        session.sync(&mut s);
        let r = session.wm.get(id).unwrap().content;
        session.input(
            &mut s,
            InputTransition::Button {
                button: 1,
                down: true,
                x: r.x + 20,
                y: r.y + 20,
            },
        );
        assert!(s.windows[&id].input.captured);
        let dock = session.dock.window_rect(id);
        session.input(
            &mut s,
            InputTransition::Motion {
                x: dock.x + 20,
                y: dock.y + 20,
            },
        );
        session.input(
            &mut s,
            InputTransition::Button {
                button: 1,
                down: false,
                x: dock.x + 20,
                y: dock.y + 20,
            },
        );
        assert!(!session.dock.modal());
        assert!(!session.wm.pointer_captured());
        assert!(!s.windows[&id].input.captured);
        assert_eq!(s.windows[&id].input.buttons, 0);
    }

    #[test]
    fn client_pixels_and_pointer_input_survive_beside_contained_dock() {
        use rvm::input_events::{EVENT_BUTTON, EVENT_MOTION, EVENT_WHEEL};

        for scale in [1, 2] {
            let mut rt = Runtime::new();
            let id = create(&mut rt, 900, 240, 2);
            let mut s = rt.graphics.shared.lock().unwrap();
            let client_colour = colour::rgba(0xa52d71);
            paint(&mut s, id, client_colour);
            let mut session = Session::default();
            session.resize(&mut s, 640, 480);
            session.density(&mut s, scale);
            session.sync(&mut s);
            session
                .command(&mut s, id, policy::WindowCommand::Move { x: 20, y: 330 })
                .unwrap();
            let content = session.wm.get(id).unwrap().content;
            let tray = session.dock.footprint();
            let (x, y) = (40, 450);
            assert!(content.contains(x, y));
            assert!(session.dock.bar().contains(x, y));
            assert!(!tray.contains(x, y));
            assert!(session.compose(&session.snapshots(&s)));
            let width = session.wm.width as u32 * scale;
            for dy in 0..scale {
                for dx in 0..scale {
                    assert_eq!(
                        session.pixels
                            [((y as u32 * scale + dy) * width + x as u32 * scale + dx) as usize],
                        client_colour,
                        "client pixels beside dock at density {scale}"
                    );
                }
            }
            let painted = (tray.x + 3, tray.y + tray.h / 2);
            assert!(content.contains(painted.0, painted.1));
            assert_ne!(
                session.pixels
                    [(painted.1 as u32 * scale * width + painted.0 as u32 * scale) as usize],
                client_colour
            );

            s.windows.get_mut(&id).unwrap().input.events.clear();
            session.input(&mut s, InputTransition::Motion { x, y });
            assert_eq!(session.hover, Some(id));
            assert!(s.windows[&id].input.inside);
            assert_eq!(s.windows[&id].input.x, x - content.x);
            assert_eq!(s.windows[&id].input.y, y - content.y);
            for down in [true, false] {
                session.input(
                    &mut s,
                    InputTransition::Button {
                        button: 0,
                        down,
                        x,
                        y,
                    },
                );
            }
            assert!(!session.wm.pointer_captured());
            assert_eq!(s.windows[&id].input.buttons, 0);
            session.input(&mut s, InputTransition::Wheel { x: 0, y: -65536 });
            let events = &s.windows[&id].input.events;
            assert!(events.iter().any(|e| e.kind == EVENT_MOTION));
            assert_eq!(events.iter().filter(|e| e.kind == EVENT_BUTTON).count(), 2);
            assert!(events
                .iter()
                .any(|e| e.kind == EVENT_WHEEL && e.wheel_y == -65536));

            s.windows.get_mut(&id).unwrap().input.events.clear();
            let icon = session.dock.window_rect(id);
            let (x, y) = (icon.x + icon.w / 2, icon.y + icon.h / 2);
            session.input(&mut s, InputTransition::Motion { x, y });
            assert_eq!(session.hover, None);
            assert!(!s.windows[&id].input.inside);
            session.input(&mut s, InputTransition::Wheel { x: 0, y: -65536 });
            for down in [true, false] {
                session.input(
                    &mut s,
                    InputTransition::Button {
                        button: 1,
                        down,
                        x,
                        y,
                    },
                );
            }
            assert!(session.dock.modal());
            assert!(s.windows[&id]
                .input
                .events
                .iter()
                .all(|e| !matches!(e.kind, EVENT_MOTION | EVENT_BUTTON | EVENT_WHEEL)));
        }
    }
    #[test]
    fn alpha_panel_reveals_underlying_white_uses_published_mask_and_preserves_capture() {
        let mut rt=Runtime::new();
        let client=create(&mut rt,500,500,2);
        let overlay=create(&mut rt,1280,800,2);
        let mut s=rt.graphics.shared.lock().unwrap();
        paint(&mut s,client,0xffffffff);
        {
            let surface=s.windows.get_mut(&overlay).unwrap();
            let frame=surface.colour.as_mut().unwrap();
            frame.back.fill(0);
            frame.back[100*1280+81]=0x80000000;
            frame.back[100*1280+82]=0xff000000;
            frame.present();
            surface.sequence+=1;
        }
        let region=Rect::new(80,100,3,1);
        s.panel=Some(panel::Panel{window:overlay,process:0,regions:vec![region,region],reserve_top:0,reserve_bottom:0,flags:panel::ALPHA});
        let mut session=Session::default();
        session.sync(&mut s);
        assert!(session.compose(&session.snapshots(&s)));
        assert_eq!(session.pixels[100*1280+80],0xffffffff);
        assert_eq!(session.pixels[100*1280+81],0xff7f7f7f);
        assert_eq!(session.pixels[100*1280+82],0xff000000);
        assert_eq!(session.panel_hit(80,100),None);
        assert_eq!(session.panel_hit(81,100),Some(overlay));
        s.windows.get_mut(&overlay).unwrap().colour.as_mut().unwrap().back[100*1280+80]=0xff000000;
        session.sync(&mut s);
        assert_eq!(session.panel_hit(80,100),None,"unpublished alpha cannot change input");
        for down in [true,false] {
            session.input(&mut s,InputTransition::Button{button:0,down,x:80,y:100});
        }
        assert!(s.windows[&client].input.events.iter().any(|e|e.kind==rvm::input_events::EVENT_BUTTON));
        session.input(&mut s,InputTransition::Button{button:0,down:true,x:81,y:100});
        assert!(s.windows[&overlay].input.captured);
        session.input(&mut s,InputTransition::Button{button:0,down:false,x:500,y:300});
        assert!(!s.windows[&overlay].input.captured);
        assert_eq!(session.panel_buttons,0);
        s.panel.as_mut().unwrap().flags|=panel::KEYBOARD|panel::MODAL;
        session.sync(&mut s);
        assert_eq!(session.panel_hit(500,300),Some(overlay),"modal dismissal owns outside clicks");
    }

}
