//! Allocation-free desktop policy and opaque packed-pixel composition.
//! No host, guest ABI, toolkit, display driver, or operating-system dependency.
//! Coordinates are logical pixels. Adapters own frames and translate actions.
pub const MAX_WINDOWS: usize = 16;
pub const TITLE: i32 = 28;
pub const BORDER: i32 = 2;
pub const TASKBAR: i32 = 34;
pub const BUTTON: i32 = 24;
pub const CONTROL_SIZE: i32 = 20;

#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}
impl Rect {
    pub const fn new(x: i32, y: i32, w: i32, h: i32) -> Self {
        Self { x, y, w, h }
    }
    pub fn contains(self, x: i32, y: i32) -> bool {
        x >= self.x && y >= self.y && x < self.x + self.w && y < self.y + self.h
    }
    pub fn intersect(self, b: Self) -> Self {
        let x = self.x.max(b.x);
        let y = self.y.max(b.y);
        Self::new(
            x,
            y,
            (self.x + self.w).min(b.x + b.w).saturating_sub(x).max(0),
            (self.y + self.h).min(b.y + b.h).saturating_sub(y).max(0),
        )
    }
    pub fn union(self, b: Self) -> Self {
        let x = self.x.min(b.x);
        let y = self.y.min(b.y);
        Self::new(
            x,
            y,
            (self.x + self.w).max(b.x + b.w) - x,
            (self.y + self.h).max(b.y + b.h) - y,
        )
    }
    pub fn empty(self) -> bool {
        self.w <= 0 || self.h <= 0
    }
}
#[derive(Clone, Copy, Default, Debug)]
pub struct Window {
    pub id: u32,
    pub content: Rect,
    pub resizable: bool,
    pub minimized: bool,
    pub maximized: bool,
    pub normal: Rect,
}
impl Window {
    pub fn chrome(self) -> Chrome {
        let title = Rect::new(
            self.content.x,
            self.content.y - TITLE + BORDER,
            self.content.w,
            TITLE - BORDER,
        );
        let top = self.content.y - TITLE + 3;
        let control = |n| {
            Rect::new(
                self.content.x + 6 + n * BUTTON,
                top,
                CONTROL_SIZE,
                TITLE - 6,
            )
            .intersect(title)
        };
        let drag = Rect::new(
            self.content.x + 6 + 3 * BUTTON + 6,
            title.y,
            (self.content.w - 6 - 3 * BUTTON - 12).max(0),
            title.h,
        );
        Chrome {
            title,
            close: control(0),
            minimize: control(1),
            maximize: control(2),
            drag,
            label: Rect::new(drag.x, title.y + 4, drag.w, 17),
            grip: if self.resizable && !self.maximized {
                Rect::new(
                    self.content.x + self.content.w - 14,
                    self.content.y + self.content.h - 14,
                    14 + BORDER,
                    14 + BORDER,
                )
            } else {
                Rect::default()
            },
        }
    }
    pub fn outer(self) -> Rect {
        Rect::new(
            self.content.x - BORDER,
            self.content.y - TITLE,
            self.content.w + 2 * BORDER,
            self.content.h + TITLE + BORDER,
        )
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Control {
    Close,
    Minimize,
    Maximize,
}
#[derive(Clone, Copy, Debug)]
pub struct Chrome {
    pub title: Rect,
    pub close: Rect,
    pub minimize: Rect,
    pub maximize: Rect,
    pub drag: Rect,
    pub label: Rect,
    pub grip: Rect,
}
impl Chrome {
    pub fn control_at(self, x: i32, y: i32) -> Option<Control> {
        if self.close.contains(x, y) {
            Some(Control::Close)
        } else if self.minimize.contains(x, y) {
            Some(Control::Minimize)
        } else if self.maximize.contains(x, y) {
            Some(Control::Maximize)
        } else {
            None
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    Focus(u32, bool),
    Resize(u32, u32, u32),
    Close(u32),
    Capture(u32, bool),
    Motion(u32, i32, i32),
    Button(u32, u16, bool, i32, i32),
}
pub struct Actions {
    items: [Option<Action>; MAX_WINDOWS + 12],
    len: usize,
}
impl Actions {
    fn new() -> Self {
        Self {
            items: [None; MAX_WINDOWS + 12],
            len: 0,
        }
    }
    fn push(&mut self, a: Action) {
        assert!(self.len < self.items.len());
        self.items[self.len] = Some(a);
        self.len += 1;
    }
    pub fn iter(&self) -> impl Iterator<Item = Action> + '_ {
        self.items[..self.len].iter().filter_map(|a| *a)
    }
}
#[derive(Clone, Copy)]
enum Capture {
    None,
    Client {
        id: u32,
        buttons: u16,
    },
    Move {
        id: u32,
        x: i32,
        y: i32,
        origin: Rect,
    },
    Resize {
        id: u32,
        x: i32,
        y: i32,
        origin: Rect,
    },
}
pub struct Desktop {
    pub width: i32,
    pub height: i32,
    pub focused: Option<u32>,
    stack: [Window; MAX_WINDOWS],
    len: usize,
    capture: Capture,
    pub pointer: (i32, i32),
    damage: [Rect; 32],
    damage_len: usize,
}
impl Desktop {
    pub fn new(width: i32, height: i32) -> Self {
        let mut s = Self {
            width: width.clamp(320, 1920),
            height: height.clamp(240, 1080),
            focused: None,
            stack: [Window::default(); MAX_WINDOWS],
            len: 0,
            capture: Capture::None,
            pointer: (0, 0),
            damage: [Rect::default(); 32],
            damage_len: 0,
        };
        s.invalidate(Rect::new(0, 0, s.width, s.height));
        s
    }
    pub fn windows(&self) -> &[Window] {
        &self.stack[..self.len]
    }
    pub fn get(&self, id: u32) -> Option<Window> {
        self.windows().iter().find(|w| w.id == id).copied()
    }
    fn index(&self, id: u32) -> Option<usize> {
        self.windows().iter().position(|w| w.id == id)
    }
    pub fn invalidate(&mut self, r: Rect) {
        let r = r.intersect(Rect::new(0, 0, self.width, self.height));
        if r.empty() {
            return;
        }
        for n in 0..self.damage_len {
            if !self.damage[n].intersect(r).empty() {
                self.damage[n] = self.damage[n].union(r);
                return;
            }
        }
        if self.damage_len == self.damage.len() {
            let mut u = r;
            for d in &self.damage {
                u = u.union(*d);
            }
            self.damage[0] = u;
            self.damage_len = 1;
        } else {
            self.damage[self.damage_len] = r;
            self.damage_len += 1;
        }
    }
    pub fn take_damage(&mut self) -> ([Rect; 32], usize) {
        let n = self.damage_len;
        self.damage_len = 0;
        (self.damage, n)
    }
    fn bar(&self) -> Rect {
        Rect::new(0, self.height - TASKBAR, self.width, TASKBAR)
    }
    pub fn task_rect(&self, index: usize) -> Rect {
        let w = ((self.width - 16) / self.len.max(1) as i32).min(160);
        Rect::new(
            8 + index as i32 * w,
            self.height - TASKBAR + 5,
            w - 4,
            TASKBAR - 10,
        )
    }
    pub fn add(&mut self, id: u32, w: u32, h: u32, resizable: bool) -> Actions {
        let mut a = Actions::new();
        if self.get(id).is_some() || self.len == MAX_WINDOWS || id == 0 {
            return a;
        }
        let i = self.len;
        let cw = if resizable {
            (w as i32).min(self.width - 40).max(64)
        } else {
            w as i32
        };
        let ch = if resizable {
            (h as i32).min(self.height - TITLE - TASKBAR - 40).max(48)
        } else {
            h as i32
        };
        let x = (22 + (i % 7) as i32 * 28).min((self.width - cw - 8).max(8));
        let y = (46 + (i % 7) as i32 * 28).min((self.height - TASKBAR - ch - 8).max(TITLE + 8));
        let win = Window {
            id,
            content: Rect::new(x, y, cw, ch),
            resizable,
            minimized: false,
            maximized: false,
            normal: Rect::new(x, y, cw, ch),
        };
        self.stack[i] = win;
        self.len += 1;
        self.invalidate(win.outer());
        self.invalidate(self.bar());
        self.focus(id, &mut a);
        if (cw, ch) != (w as i32, h as i32) {
            a.push(Action::Resize(id, cw as u32, ch as u32));
        }
        a
    }
    pub fn remove(&mut self, id: u32) -> Actions {
        let mut a = Actions::new();
        let Some(i) = self.index(id) else {
            return a;
        };
        self.cancel_capture(id, &mut a);
        self.invalidate(self.stack[i].outer());
        for j in i..self.len - 1 {
            self.stack[j] = self.stack[j + 1];
        }
        self.len -= 1;
        if self.focused == Some(id) {
            a.push(Action::Focus(id, false));
            self.focused = None;
            if let Some(w) = self.windows().iter().rev().find(|w| !w.minimized).copied() {
                self.focus(w.id, &mut a);
            }
        }
        self.invalidate(self.bar());
        a
    }
    pub fn accepted(&mut self, id: u32, width: u32, height: u32, resizable: bool) {
        let Some(i) = self.index(id) else {
            return;
        };
        let old = self.stack[i];
        self.stack[i].resizable = resizable;
        if !resizable && self.stack[i].maximized {
            self.stack[i].maximized = false;
            self.stack[i].content = self.clamp_normal(self.stack[i].normal);
        }
        if !matches!(self.capture,Capture::Resize{id:n,..}if n==id) {
            self.stack[i].content.w = width as i32;
            self.stack[i].content.h = height as i32;
        }
        if !self.stack[i].maximized {
            self.stack[i].normal = self.stack[i].content;
        }
        self.invalidate(old.outer());
        self.invalidate(self.stack[i].outer());
    }
    pub fn maximized_bounds(&self) -> Rect {
        Rect::new(
            BORDER,
            TITLE,
            self.width - 2 * BORDER,
            self.height - TITLE - TASKBAR - BORDER,
        )
    }
    fn clamp_normal(&self, mut r: Rect) -> Rect {
        r.x = r.x.clamp(8, (self.width - 80).max(8));
        r.y =
            r.y.clamp(TITLE + 8, (self.height - TASKBAR - 40).max(TITLE + 8));
        r
    }
    fn toggle_maximize(&mut self, id: u32, a: &mut Actions) {
        let Some(i) = self.index(id) else {
            return;
        };
        if !self.stack[i].resizable {
            return;
        }
        let old = self.stack[i];
        self.cancel_capture(id, a);
        if old.maximized {
            self.stack[i].maximized = false;
            self.stack[i].content = self.clamp_normal(old.normal);
        } else {
            self.stack[i].normal = old.content;
            self.stack[i].maximized = true;
            self.stack[i].content = self.maximized_bounds();
        }
        let now = self.stack[i];
        self.invalidate(old.outer());
        self.invalidate(now.outer());
        a.push(Action::Resize(
            id,
            now.content.w as u32,
            now.content.h as u32,
        ));
    }
    fn focus(&mut self, id: u32, a: &mut Actions) {
        let Some(i) = self.index(id) else {
            return;
        };
        if let Some(old) = self.focused {
            if old != id {
                self.cancel_capture(old, a);
                a.push(Action::Focus(old, false));
                if let Some(w) = self.get(old) {
                    self.invalidate(w.outer());
                }
            }
        }
        let mut w = self.stack[i];
        w.minimized = false;
        for j in i..self.len - 1 {
            self.stack[j] = self.stack[j + 1];
        }
        self.stack[self.len - 1] = w;
        self.focused = Some(id);
        self.invalidate(w.outer());
        self.invalidate(self.bar());
        a.push(Action::Focus(id, true));
    }
    pub fn activate(&mut self, id: u32) -> Actions {
        let mut a = Actions::new();
        self.focus(id, &mut a);
        a
    }
    fn cancel_capture(&mut self, id: u32, a: &mut Actions) {
        match self.capture {
            Capture::Client { id: n, .. } if n == id => {
                a.push(Action::Capture(id, false));
                self.capture = Capture::None;
            }
            Capture::Move { id: n, .. } | Capture::Resize { id: n, .. } if n == id => {
                self.capture = Capture::None
            }
            _ => {}
        }
    }
    pub fn lose_focus(&mut self) -> Actions {
        let mut a = Actions::new();
        if let Some(id) = self.focused {
            self.cancel_capture(id, &mut a);
            a.push(Action::Focus(id, false));
        }
        self.capture = Capture::None;
        a
    }
    pub fn host_resize(&mut self, w: u32, h: u32) -> Actions {
        let mut a = Actions::new();
        self.width = (w as i32).clamp(320, 1920);
        self.height = (h as i32).clamp(240, 1080);
        for i in 0..self.len {
            if self.stack[i].maximized {
                let r = self.maximized_bounds();
                self.stack[i].content = r;
                a.push(Action::Resize(self.stack[i].id, r.w as u32, r.h as u32));
            } else {
                self.stack[i].content = self.clamp_normal(self.stack[i].content);
                self.stack[i].normal = self.stack[i].content;
            }
        }
        self.invalidate(Rect::new(0, 0, self.width, self.height));
        a
    }
    pub fn hit_client(&self, x: i32, y: i32) -> Option<u32> {
        if let Capture::Client { id, .. } = self.capture {
            return Some(id);
        }
        self.windows()
            .iter()
            .rev()
            .find(|w| !w.minimized && w.outer().contains(x, y))
            .and_then(|w| w.content.contains(x, y).then_some(w.id))
    }
    pub fn hovered_control(&self) -> Option<(u32, Control)> {
        if !matches!(self.capture, Capture::None) {
            return None;
        }
        self.windows()
            .iter()
            .rev()
            .find(|w| !w.minimized && w.outer().contains(self.pointer.0, self.pointer.1))
            .and_then(|w| {
                w.chrome()
                    .control_at(self.pointer.0, self.pointer.1)
                    .map(|c| (w.id, c))
            })
    }
    pub fn motion(&mut self, x: i32, y: i32) -> Actions {
        let previous = self.hovered_control();
        self.pointer = (x, y);
        let current = self.hovered_control();
        if previous != current {
            for (id, _) in previous.into_iter().chain(current) {
                if let Some(w) = self.get(id) {
                    self.invalidate(w.chrome().title);
                }
            }
        }
        let mut a = Actions::new();
        match self.capture {
            Capture::Move {
                id,
                x: ox,
                y: oy,
                origin,
            } => {
                if let Some(i) = self.index(id) {
                    let old = self.stack[i].outer();
                    self.stack[i].content.x = (origin.x.saturating_add(x.saturating_sub(ox)))
                        .clamp(8, (self.width - 80).max(8));
                    self.stack[i].content.y = (origin.y.saturating_add(y.saturating_sub(oy)))
                        .clamp(TITLE + 8, (self.height - TASKBAR - 40).max(TITLE + 8));
                    self.stack[i].normal = self.stack[i].content;
                    self.invalidate(old);
                    self.invalidate(self.stack[i].outer());
                }
            }
            Capture::Resize {
                id,
                x: ox,
                y: oy,
                origin,
            } => {
                if let Some(i) = self.index(id) {
                    let old = self.stack[i].outer();
                    let w = (origin.w.saturating_add(x.saturating_sub(ox))).clamp(64, 1920);
                    let h = (origin.h.saturating_add(y.saturating_sub(oy))).clamp(48, 1080);
                    self.stack[i].content.w = w;
                    self.stack[i].content.h = h;
                    self.stack[i].normal = self.stack[i].content;
                    self.invalidate(old);
                    self.invalidate(self.stack[i].outer());
                    a.push(Action::Resize(id, w as u32, h as u32));
                }
            }
            _ => {
                if let Some(id) = self.hit_client(x, y) {
                    if let Some(w) = self.get(id) {
                        a.push(Action::Motion(
                            id,
                            x.saturating_sub(w.content.x),
                            y.saturating_sub(w.content.y),
                        ));
                    }
                }
            }
        }
        a
    }
    pub fn button(&mut self, button: u16, down: bool) -> Actions {
        let mut a = Actions::new();
        if button >= 16 {
            return a;
        }
        let (x, y) = self.pointer;
        if let Capture::Client { id, mut buttons } = self.capture {
            if down {
                buttons |= 1 << button;
            } else {
                buttons &= !(1 << button);
            }
            if let Some(w) = self.get(id) {
                a.push(Action::Button(
                    id,
                    button,
                    down,
                    x.saturating_sub(w.content.x),
                    y.saturating_sub(w.content.y),
                ));
            }
            if buttons == 0 {
                self.capture = Capture::None;
                a.push(Action::Capture(id, false));
            } else {
                self.capture = Capture::Client { id, buttons };
            }
            return a;
        }
        if !down {
            if button == 0 {
                self.capture = Capture::None;
            }
            return a;
        }
        if !matches!(self.capture, Capture::None) {
            return a;
        }
        if self.bar().contains(x, y) {
            if button == 0 {
                if let Some(i) = (0..self.len).find(|&i| self.task_rect(i).contains(x, y)) {
                    self.focus(self.stack[i].id, &mut a);
                }
            }
            return a;
        }
        let Some(w) = self
            .windows()
            .iter()
            .rev()
            .find(|w| !w.minimized && w.outer().contains(x, y))
            .copied()
        else {
            if let Some(id) = self.focused.take() {
                a.push(Action::Focus(id, false));
                self.invalidate(self.get(id).unwrap().outer());
            }
            return a;
        };
        self.focus(w.id, &mut a);
        if button == 0 && w.chrome().grip.contains(x, y) {
            self.capture = Capture::Resize {
                id: w.id,
                x,
                y,
                origin: w.content,
            };
            return a;
        }
        if w.content.contains(x, y) {
            self.capture = Capture::Client {
                id: w.id,
                buttons: 1 << button,
            };
            a.push(Action::Motion(
                w.id,
                x.saturating_sub(w.content.x),
                y.saturating_sub(w.content.y),
            ));
            a.push(Action::Capture(w.id, true));
            a.push(Action::Button(
                w.id,
                button,
                true,
                x.saturating_sub(w.content.x),
                y.saturating_sub(w.content.y),
            ));
            return a;
        }
        if button != 0 {
            return a;
        }
        let chrome = w.chrome();
        match chrome.control_at(x, y) {
            Some(Control::Close) => a.push(Action::Close(w.id)),
            Some(Control::Minimize) => {
                let i = self.index(w.id).unwrap();
                self.stack[i].minimized = true;
                self.invalidate(w.outer());
                self.focused = None;
                a.push(Action::Focus(w.id, false));
                if let Some(next) = self.windows().iter().rev().find(|w| !w.minimized).copied() {
                    self.focus(next.id, &mut a);
                }
                self.invalidate(self.bar());
            }
            Some(Control::Maximize) => self.toggle_maximize(w.id, &mut a),
            None => {
                if chrome.drag.contains(x, y) {
                    let i = self.index(w.id).unwrap();
                    if w.maximized {
                        // Restore under the pointer, retaining its relative titlebar
                        // anchor. Moving then behaves exactly like a normal window.
                        let mut r = w.normal;
                        let anchor = ((x - w.content.x) as i64 * r.w as i64
                            / w.content.w.max(1) as i64)
                            as i32;
                        r.x = x.saturating_sub(anchor.clamp(20, (r.w - 20).max(20)));
                        r.y = y.saturating_add(w.content.y - y);
                        self.stack[i].content = self.clamp_normal(r);
                        self.stack[i].normal = self.stack[i].content;
                        self.stack[i].maximized = false;
                        let now = self.stack[i];
                        self.invalidate(w.outer());
                        self.invalidate(now.outer());
                        a.push(Action::Resize(
                            w.id,
                            now.content.w as u32,
                            now.content.h as u32,
                        ));
                    }
                    self.capture = Capture::Move {
                        id: w.id,
                        x,
                        y,
                        origin: self.stack[i].content,
                    };
                }
            }
        }
        a
    }
}

pub enum Pixels<'a> {
    Rgba(&'a [u32]),
    Rgb565(&'a [u16]),
}
fn rgba565(c: u16) -> u32 {
    let r = (c >> 11) & 31;
    let g = (c >> 5) & 63;
    let b = c & 31;
    u32::from_le_bytes([
        ((r << 3) | (r >> 2)) as u8,
        ((g << 2) | (g >> 4)) as u8,
        ((b << 3) | (b >> 2)) as u8,
        255,
    ])
}
/// Paint an opaque logical rectangle, clipped to damage, at 1x or 2x density.
pub fn fill(out: &mut [u32], width: u32, height: u32, scale: u32, r: Rect, damage: Rect, c: u32) {
    if !(1..=2).contains(&scale) || out.len() != width as usize * height as usize {
        return;
    }
    let r = r.intersect(damage).intersect(Rect::new(
        0,
        0,
        (width / scale) as i32,
        (height / scale) as i32,
    ));
    if r.empty() {
        return;
    }
    for y in r.y as u32 * scale..(r.y + r.h) as u32 * scale {
        let at = (y * width + r.x as u32 * scale) as usize;
        out[at..at + r.w as usize * scale as usize].fill(c);
    }
}
/// Native packed frames are borrowed only for this copy. No client pointers or
/// destination buffers are retained; immutable published frames remain intact.
pub fn blit(
    out: &mut [u32],
    width: u32,
    height: u32,
    scale: u32,
    dest: Rect,
    damage: Rect,
    source: Pixels<'_>,
    sw: u32,
    sh: u32,
) {
    if !(1..=2).contains(&scale)
        || out.len() != width as usize * height as usize
        || dest.empty()
        || sw == 0
        || sh == 0
    {
        return;
    }
    let len = match &source {
        Pixels::Rgba(p) => p.len(),
        Pixels::Rgb565(p) => p.len(),
    };
    if len != sw as usize * sh as usize {
        return;
    }
    let r = dest.intersect(damage).intersect(Rect::new(
        0,
        0,
        (width / scale) as i32,
        (height / scale) as i32,
    ));
    if r.empty() {
        return;
    }
    for y in r.y as u32 * scale..(r.y + r.h) as u32 * scale {
        let sy = ((y as i64 - dest.y as i64 * scale as i64) * sh as i64
            / (dest.h as i64 * scale as i64)) as usize;
        let at = (y * width + r.x as u32 * scale) as usize;
        let n = r.w as usize * scale as usize;
        if let Pixels::Rgba(p) = &source {
            if sw == dest.w as u32 * scale && sh == dest.h as u32 * scale {
                let sx = (r.x - dest.x) as usize * scale as usize;
                out[at..at + n]
                    .copy_from_slice(&p[sy * sw as usize + sx..sy * sw as usize + sx + n]);
                continue;
            }
        }
        for x in 0..n {
            let sx = ((r.x as i64 * scale as i64 + x as i64 - dest.x as i64 * scale as i64)
                * sw as i64
                / (dest.w as i64 * scale as i64)) as usize;
            let i = sy * sw as usize + sx;
            out[at + x] = match &source {
                Pixels::Rgba(p) => p[i],
                Pixels::Rgb565(p) => rgba565(p[i]),
            };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn stacking_capture_and_focus() {
        let mut d = Desktop::new(800, 600);
        d.add(1, 300, 200, true);
        d.add(2, 300, 200, true);
        let w = d.get(1).unwrap();
        d.motion(w.content.x + 5, w.content.y + 5);
        let a = d.button(0, true);
        assert_eq!(d.focused, Some(1));
        assert!(a.iter().any(|a| a == Action::Capture(1, true)));
        d.motion(-90, -80);
        assert_eq!(d.hit_client(-90, -80), Some(1));
        let a = d.button(0, false);
        assert!(a.iter().any(|a| a == Action::Capture(1, false)));
    }
    #[test]
    fn minimize_restore_close_and_teardown() {
        let mut d = Desktop::new(800, 600);
        d.add(1, 200, 100, true);
        d.add(2, 200, 100, true);
        let w = d.get(2).unwrap();
        let r = w.chrome().minimize;
        d.motion(r.x + r.w / 2, r.y + r.h / 2);
        d.button(0, true);
        d.button(0, false);
        assert!(d.get(2).unwrap().minimized);
        assert_eq!(d.focused, Some(1));
        let i = d.windows().iter().position(|w| w.id == 2).unwrap();
        let r = d.task_rect(i);
        d.motion(r.x + 3, r.y + 3);
        d.button(0, true);
        assert!(!d.get(2).unwrap().minimized);
        assert_eq!(d.focused, Some(2));
        d.remove(2);
        assert_eq!(d.focused, Some(1));
        assert!(d.get(2).is_none());
    }
    #[test]
    fn resize_drag_damage_and_idle() {
        let mut d = Desktop::new(800, 600);
        d.add(1, 200, 100, true);
        d.take_damage();
        assert_eq!(d.take_damage().1, 0);
        let w = d.get(1).unwrap();
        d.motion(w.content.x + w.content.w, w.content.y + w.content.h);
        d.button(0, true);
        let a = d.motion(
            w.content.x + w.content.w + 30,
            w.content.y + w.content.h + 40,
        );
        assert!(a.iter().any(|a| a == Action::Resize(1, 230, 140)));
        assert!(d.take_damage().1 > 0);
        d.button(0, false);
        d.accepted(1, 220, 130, true);
        assert_eq!(d.get(1).unwrap().content.w, 220);
    }
    #[test]
    fn opaque_clip_density_and_immutable_source() {
        let source = vec![0xff001122, 0xff334455, 0xff667788, 0xff99aabb];
        let original = source.clone();
        let mut out = vec![0; 64];
        blit(
            &mut out,
            8,
            8,
            2,
            Rect::new(1, 1, 2, 2),
            Rect::new(2, 1, 1, 2),
            Pixels::Rgba(&source),
            2,
            2,
        );
        assert_eq!(out[2 * 8 + 4], source[1]);
        assert_eq!(out[5 * 8 + 5], source[3]);
        assert_eq!(out[2 * 8 + 3], 0);
        assert_eq!(source, original);
    }
    #[test]
    fn rgb565_expands_exact_primaries() {
        let mut out = [0; 3];
        blit(
            &mut out,
            3,
            1,
            1,
            Rect::new(0, 0, 3, 1),
            Rect::new(0, 0, 3, 1),
            Pixels::Rgb565(&[0xf800, 0x07e0, 0x001f]),
            3,
            1,
        );
        assert_eq!(out, [0xff0000ff, 0xff00ff00, 0xffff0000]);
    }
    fn control(d: &mut Desktop, id: u32, which: Control) -> Actions {
        let c = d.get(id).unwrap().chrome();
        let r = match which {
            Control::Close => c.close,
            Control::Minimize => c.minimize,
            Control::Maximize => c.maximize,
        };
        d.motion(r.x + r.w / 2, r.y + r.h / 2);
        let a = d.button(0, true);
        d.button(0, false);
        a
    }
    #[test]
    fn left_controls_share_geometry_and_fixed_maximize_is_disabled() {
        let mut d = Desktop::new(800, 600);
        d.add(1, 300, 200, false);
        let w = d.get(1).unwrap();
        let c = w.chrome();
        assert!(c.close.x < c.minimize.x && c.minimize.x < c.maximize.x && c.maximize.x < c.drag.x);
        for (r, k) in [
            (c.close, Control::Close),
            (c.minimize, Control::Minimize),
            (c.maximize, Control::Maximize),
        ] {
            assert_eq!(c.control_at(r.x + r.w / 2, r.y + r.h / 2), Some(k));
            assert_eq!(r.intersect(c.title), r);
        }
        let a = control(&mut d, 1, Control::Maximize);
        assert!(!a.iter().any(|a| matches!(a, Action::Resize(..))));
        assert!(!d.get(1).unwrap().maximized);
        assert_eq!(d.get(1).unwrap().content, w.content);
        d.motion(c.close.x + 3, c.close.y + 3);
        assert!(!d
            .button(1, true)
            .iter()
            .any(|a| matches!(a, Action::Close(_))));
        d.button(1, false);
        let a = control(&mut d, 1, Control::Close);
        assert!(a.iter().any(|a| a == Action::Close(1)));
        assert!(!d.get(1).unwrap().minimized);
        assert_eq!(d.get(1).unwrap().content, w.content);
        assert_eq!(d.button(16, true).iter().count(), 0);
    }
    #[test]
    fn maximize_restore_and_minimized_workspace_resize_preserve_normal_bounds() {
        let mut d = Desktop::new(800, 600);
        d.add(1, 300, 200, true);
        d.add(2, 180, 120, true);
        let normal = d.get(1).unwrap().content;
        let a = control(&mut d, 1, Control::Maximize);
        assert!(a.iter().any(|a| a == Action::Resize(1, 796, 536)));
        let w = d.get(1).unwrap();
        assert!(w.maximized);
        assert_eq!(w.normal, normal);
        assert_eq!(w.outer(), Rect::new(0, 0, 800, 566));
        d.accepted(1, 796, 536, true);
        assert_eq!(d.get(1).unwrap().normal, normal);
        control(&mut d, 1, Control::Minimize);
        assert!(d.get(1).unwrap().minimized);
        assert!(d.get(1).unwrap().maximized);
        assert_eq!(d.focused, Some(2));
        let a = d.host_resize(960, 720);
        assert!(a.iter().any(|a| a == Action::Resize(1, 956, 656)));
        assert_eq!(d.get(1).unwrap().content, d.maximized_bounds());
        let i = d.windows().iter().position(|w| w.id == 1).unwrap();
        let r = d.task_rect(i);
        d.motion(r.x + 2, r.y + 2);
        d.button(0, true);
        d.button(0, false);
        assert!(!d.get(1).unwrap().minimized);
        assert!(d.get(1).unwrap().maximized);
        let a = control(&mut d, 1, Control::Maximize);
        assert!(a
            .iter()
            .any(|a| a == Action::Resize(1, normal.w as u32, normal.h as u32)));
        assert_eq!(d.get(1).unwrap().content, normal);
        assert!(!d.get(1).unwrap().maximized);
    }
    #[test]
    fn dragging_maximized_title_restores_under_pointer_then_moves_normally() {
        let mut d = Desktop::new(900, 650);
        d.add(1, 320, 240, true);
        let normal = d.get(1).unwrap().content;
        control(&mut d, 1, Control::Maximize);
        let drag = d.get(1).unwrap().chrome().drag;
        let (x, y) = (drag.x + 30, drag.y + drag.h / 2);
        d.motion(x, y);
        let a = d.button(0, true);
        let restored = d.get(1).unwrap();
        assert!(!restored.maximized);
        assert_eq!(
            (restored.content.w, restored.content.h),
            (normal.w, normal.h)
        );
        assert!(a.iter().any(|a| a == Action::Resize(1, 320, 240)));
        d.motion(x + 60, y + 40);
        d.button(0, false);
        let moved = d.get(1).unwrap();
        assert_eq!(moved.content.x, restored.content.x + 60);
        assert_eq!(moved.content.y, restored.content.y + 40);
        assert_eq!(moved.normal, moved.content);
        control(&mut d, 1, Control::Maximize);
        control(&mut d, 1, Control::Maximize);
        assert_eq!(d.get(1).unwrap().content, moved.content);
    }
    #[test]
    fn resizing_workspace_handles_all_maximized_windows_without_action_overflow() {
        let mut d = Desktop::new(800, 600);
        for id in 1..=MAX_WINDOWS as u32 {
            d.add(id, 200, 100, true);
            control(&mut d, id, Control::Maximize);
        }
        let a = d.host_resize(1920, 1080);
        assert_eq!(
            a.iter().filter(|a| matches!(a, Action::Resize(..))).count(),
            MAX_WINDOWS
        );
        assert!(d
            .windows()
            .iter()
            .all(|w| w.maximized && w.content == d.maximized_bounds()));
    }
}
