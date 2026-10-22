//! First FuseDE dock view/controller. No toolkit, host or guest ABI dependency.
//! The adapter supplies complete window snapshots and applies ordinary commands.
use super::{
    dock::{self, Dock, Group, GroupKey, WindowRecord},
    icons,
    policy::{self, Rect, WindowCommand},
    text,
};

pub const HEIGHT: i32 = 64;
const SLOT: i32 = 54;
const ROW: i32 = 28;
const HEADER: i32 = 30;
#[derive(Clone, Debug)]
pub struct Entry {
    pub window: u32,
    pub command: &'static str,
    pub title: String,
    pub rect: Rect,
    pub disabled: bool,
    index: usize,
}
#[derive(Default)]
pub struct Interaction {
    pub consumed: bool,
    pub requests: Vec<(u32, WindowCommand)>,
}
pub struct View {
    pub model: Dock,
    pub menu: Option<GroupKey>,
    pub offset: usize,
    pub scroll: usize,
    selected: usize,
    pointer: (i32, i32),
    hover: Option<GroupKey>,
    swallowed_buttons: u16,
    swallowed_keys: [bool; 256],
    width: i32,
    height: i32,
}
impl View {
    pub fn new(width: i32, height: i32) -> Self {
        Self {
            model: Dock::new(),
            menu: None,
            offset: 0,
            scroll: 0,
            selected: 0,
            pointer: (-1, -1),
            hover: None,
            swallowed_buttons: 0,
            swallowed_keys: [false; 256],
            width,
            height,
        }
    }
    pub fn bar(&self) -> Rect {
        Rect::new(0, self.height - HEIGHT, self.width, HEIGHT)
    }
    fn capacity(&self) -> usize {
        ((self.width - 72) / SLOT).max(1) as usize
    }
    fn visible_count(&self) -> usize {
        self.model.groups().len().min(self.capacity())
    }
    fn overflowing(&self) -> bool {
        self.model.groups().len() > self.capacity()
    }
    fn tray(&self) -> Rect {
        let width =
            (self.visible_count() as i32 * SLOT + 16 + if self.overflowing() { 48 } else { 0 })
                .max(80);
        Rect::new((self.width - width) / 2, self.height - 60, width, 56)
    }
    /// The floating panel's painted/input footprint. `bar()` is only the
    /// work-area reservation and conservative damage, not an opaque surface.
    pub fn footprint(&self) -> Rect {
        self.tray()
    }
    pub fn group_rect(&self, key: &GroupKey) -> Rect {
        let Some(index) = self.model.groups().iter().position(|g| &g.key == key) else {
            return Rect::default();
        };
        if index < self.offset || index >= self.offset + self.visible_count() {
            return Rect::default();
        }
        let tray = self.tray();
        Rect::new(
            tray.x
                + 8
                + if self.overflowing() { 24 } else { 0 }
                + (index - self.offset) as i32 * SLOT,
            tray.y + 4,
            SLOT - 4,
            48,
        )
    }
    pub fn window_rect(&self, id: u32) -> Rect {
        self.model
            .groups()
            .iter()
            .find(|g| g.windows.iter().any(|w| w.id == id))
            .map_or(Rect::default(), |g| self.group_rect(&g.key))
    }
    fn group_at(&self, x: i32, y: i32) -> Option<GroupKey> {
        self.model
            .groups()
            .iter()
            .find(|g| self.group_rect(&g.key).contains(x, y))
            .map(|g| g.key.clone())
    }
    fn menu_group(&self) -> Option<&Group> {
        self.menu.as_ref().and_then(|key| self.model.group(key))
    }
    fn row_capacity(&self) -> usize {
        ((self.height - HEIGHT - 24 - HEADER - 3 * ROW) / ROW).max(1) as usize
    }
    pub fn menu_rect(&self) -> Rect {
        let Some(group) = self.menu_group() else {
            return Rect::default();
        };
        let rows = group.windows.len().min(self.row_capacity());
        let width = (self.width - 16).min(340);
        let anchor = self.group_rect(&group.key);
        let x = (anchor.x + anchor.w / 2 - width / 2).clamp(8, (self.width - width - 8).max(8));
        let height = HEADER + (rows as i32 + 3) * ROW + 8;
        Rect::new(x, (self.height - HEIGHT - height - 4).max(4), width, height)
    }
    pub fn entries(&self) -> Vec<Entry> {
        let Some(group) = self.menu_group() else {
            return Vec::new();
        };
        let r = self.menu_rect();
        let mut entries = Vec::new();
        for (index, window) in group
            .windows
            .iter()
            .enumerate()
            .skip(self.scroll)
            .take(self.row_capacity())
        {
            let duplicate = group
                .windows
                .iter()
                .filter(|w| w.title == window.title)
                .count()
                > 1;
            let mut title = if window.title.is_empty() {
                format!("Window {}", window.id)
            } else {
                window.title.clone()
            };
            if duplicate {
                title.push_str(&format!(" · {}", window.id));
            }
            if window.close_pending {
                title.push_str("  (closing)");
            } else if window.minimized {
                title.push_str("  (minimized)");
            }
            entries.push(Entry {
                window: window.id,
                command: "activate",
                title,
                rect: Rect::new(
                    r.x + 4,
                    r.y + HEADER + (index - self.scroll) as i32 * ROW,
                    r.w - 8,
                    ROW,
                ),
                disabled: false,
                index,
            });
        }
        let base = entries.len();
        for (n, (command, title, disabled)) in [
            (
                "minimize_all",
                "Minimize all windows",
                group.all_minimized(),
            ),
            ("restore_all", "Show all windows", false),
            (
                "close_all",
                "Close all windows",
                group.windows.iter().all(|w| w.close_pending),
            ),
        ]
        .into_iter()
        .enumerate()
        {
            entries.push(Entry {
                window: 0,
                command,
                title: title.into(),
                rect: Rect::new(
                    r.x + 4,
                    r.y + HEADER + (base + n) as i32 * ROW,
                    r.w - 8,
                    ROW,
                ),
                disabled,
                index: group.windows.len() + n,
            });
        }
        entries
    }
    pub fn bounds(&self) -> Rect {
        let mut r = self.bar();
        if self.menu_group().is_some() {
            r = r.union(self.menu_rect());
        } else if self.hover.is_some() {
            r = r.union(self.tooltip_rect());
        }
        r
    }
    fn tooltip_rect(&self) -> Rect {
        let Some(group) = self.hover.as_ref().and_then(|k| self.model.group(k)) else {
            return Rect::default();
        };
        let anchor = self.group_rect(&group.key);
        let width =
            (text::measure(&group.name, 1) as i32 + 24).clamp(72, (self.width - 16).max(72));
        Rect::new(
            (anchor.x + anchor.w / 2 - width / 2).clamp(8, (self.width - width - 8).max(8)),
            self.height - HEIGHT - 28,
            width,
            24,
        )
    }
    pub fn modal(&self) -> bool {
        self.menu_group().is_some()
    }
    pub fn covers(&self, x: i32, y: i32) -> bool {
        self.footprint().contains(x, y) || self.modal() && self.menu_rect().contains(x, y)
    }
    pub fn synchronize(&mut self, records: &[WindowRecord<'_>], width: i32, height: i32) -> bool {
        let mut changed = self.model.synchronize(records);
        if (width, height) != (self.width, self.height) {
            self.width = width;
            self.height = height;
            changed = true;
        }
        self.offset = self
            .offset
            .min(self.model.groups().len().saturating_sub(self.capacity()));
        if let Some(count) = self.menu_group().map(|group| group.windows.len()) {
            self.selected = self.selected.min(count + 2);
            self.scroll = self.scroll.min(count.saturating_sub(self.row_capacity()));
        } else if self.menu.take().is_some() {
            self.scroll = 0;
            changed = true;
        }
        let hover = self.group_at(self.pointer.0, self.pointer.1);
        if self.hover != hover {
            self.hover = hover;
            changed = true;
        }
        changed
    }
    pub fn motion(&mut self, x: i32, y: i32) -> bool {
        let old = self.hover.clone();
        let old_selected = self.selected;
        let before = self
            .entries()
            .iter()
            .find(|e| e.rect.contains(self.pointer.0, self.pointer.1))
            .map(|e| e.index);
        self.pointer = (x, y);
        self.hover = self.group_at(x, y);
        let after = self
            .entries()
            .iter()
            .find(|e| e.rect.contains(x, y))
            .map(|e| e.index);
        if let Some(index) = after {
            self.selected = index;
        }
        old != self.hover || before != after || old_selected != self.selected
    }
    pub fn dismiss(&mut self) {
        self.menu = None;
        self.scroll = 0;
    }
    pub fn owns_button(&self, button: u16) -> bool {
        button < 16 && self.swallowed_buttons & (1 << button) != 0
    }
    pub fn lost_focus(&mut self) {
        self.dismiss();
        self.hover = None;
        self.swallowed_buttons = 0;
        self.swallowed_keys.fill(false);
    }
    fn open(&mut self, key: GroupKey) {
        self.menu = Some(key);
        self.scroll = 0;
        self.selected = self
            .menu_group()
            .and_then(|g| g.windows.iter().position(|w| w.focused))
            .unwrap_or(0);
        self.reveal_selected();
    }
    fn reveal_selected(&mut self) {
        if self.selected < self.menu_group().map_or(0, |g| g.windows.len()) {
            if self.selected < self.scroll {
                self.scroll = self.selected;
            }
            if self.selected >= self.scroll + self.row_capacity() {
                self.scroll = self.selected + 1 - self.row_capacity();
            }
        }
    }
    fn invoke(&mut self, index: usize) -> Vec<(u32, WindowCommand)> {
        let Some(group) = self.menu_group() else {
            return Vec::new();
        };
        let windows = &group.windows;
        if index == windows.len() && group.all_minimized()
            || index == windows.len() + 2 && windows.iter().all(|w| w.close_pending)
        {
            return Vec::new();
        }
        let result = if let Some(window) = windows.get(index) {
            vec![(window.id, WindowCommand::Activate)]
        } else {
            let command = match index - windows.len() {
                0 => WindowCommand::Minimize,
                1 => WindowCommand::Activate,
                2 => WindowCommand::Close,
                _ => return Vec::new(),
            };
            windows
                .iter()
                .filter(|w| command != WindowCommand::Close || !w.close_pending)
                .map(|w| (w.id, command))
                .collect()
        };
        self.dismiss();
        result
    }
    pub fn button(&mut self, button: u16, down: bool, x: i32, y: i32) -> Interaction {
        let mut out = Interaction::default();
        if button >= 16 {
            return out;
        }
        if !down && self.swallowed_buttons & (1 << button) != 0 {
            self.swallowed_buttons &= !(1 << button);
            out.consumed = true;
            return out;
        }
        if !down {
            return out;
        }
        // Keep a dock press/release sequence in the dock, including chorded
        // buttons and repeated press notifications, until all buttons release.
        if self.swallowed_buttons != 0 {
            self.swallowed_buttons |= 1 << button;
            out.consumed = true;
            return out;
        }
        if !self.covers(x, y) && !self.modal() {
            return out;
        }
        self.swallowed_buttons |= 1 << button;
        out.consumed = true;
        if let Some(key) = self.group_at(x, y) {
            if button == 1
                || button == 0 && matches!(self.model.click(&key), Some(dock::Click::Menu))
            {
                if self.menu.as_ref() == Some(&key) {
                    self.dismiss()
                } else {
                    self.open(key)
                }
            } else if button == 0 {
                self.dismiss();
                if let Some(dock::Click::Activate(id)) = self.model.click(&key) {
                    out.requests.push((id, WindowCommand::Activate));
                }
            }
            return out;
        }
        if self.modal() {
            if button == 0 {
                if let Some(entry) = self.entries().into_iter().find(|e| e.rect.contains(x, y)) {
                    if !entry.disabled {
                        out.requests = self.invoke(entry.index);
                    }
                    return out;
                }
            }
            self.dismiss();
            return out;
        }
        if self.overflowing() && button == 0 {
            let tray = self.tray();
            if Rect::new(tray.x + 4, tray.y + 4, 24, tray.h - 8).contains(x, y) {
                self.offset = self.offset.saturating_sub(1);
            } else if Rect::new(tray.x + tray.w - 28, tray.y + 4, 24, tray.h - 8).contains(x, y) {
                self.offset = (self.offset + 1).min(self.model.groups().len() - self.capacity());
            }
            self.hover = self.group_at(x, y);
        }
        out
    }
    pub fn wheel(&mut self, x: i32, y: i32, delta: i32) -> bool {
        if delta == 0 {
            return self.modal() || self.footprint().contains(x, y);
        }
        if self.modal() {
            if !self.menu_rect().contains(x, y) {
                return true;
            }
            let max = self
                .menu_group()
                .map_or(0, |g| g.windows.len().saturating_sub(self.row_capacity()));
            self.scroll = if delta > 0 {
                self.scroll.saturating_sub(1)
            } else {
                (self.scroll + 1).min(max)
            };
            return true;
        }
        if self.footprint().contains(x, y) {
            let max = self.model.groups().len().saturating_sub(self.capacity());
            self.offset = if delta > 0 {
                self.offset.saturating_sub(1)
            } else {
                (self.offset + 1).min(max)
            };
            self.hover = self.group_at(self.pointer.0, self.pointer.1);
            return true;
        }
        false
    }
    pub fn key(&mut self, key: u16, down: bool) -> Interaction {
        let mut out = Interaction::default();
        if key >= 256 {
            return out;
        }
        if !down && self.swallowed_keys[key as usize] {
            self.swallowed_keys[key as usize] = false;
            out.consumed = true;
            return out;
        }
        // A release belongs to whoever received its press. Repeats of a press
        // consumed while dismissing the menu must not leak to the application.
        if !down {
            return out;
        }
        if !self.modal() && self.swallowed_keys[key as usize] {
            out.consumed = true;
            return out;
        }
        if !self.modal() {
            return out;
        }
        out.consumed = true;
        if down {
            self.swallowed_keys[key as usize] = true;
            let total = self.menu_group().unwrap().windows.len() + 3;
            match key {
                41 => self.dismiss(),
                40 | 44 => out.requests = self.invoke(self.selected),
                82 => {
                    self.selected = (self.selected + total - 1) % total;
                    self.reveal_selected();
                }
                81 | 43 => {
                    self.selected = (self.selected + 1) % total;
                    self.reveal_selected();
                }
                74 => {
                    self.selected = 0;
                    self.reveal_selected();
                }
                77 => {
                    self.selected = total - 1;
                }
                _ => {}
            }
        }
        out
    }
    pub fn suppress_text(&self) -> bool {
        self.modal() || self.swallowed_keys.iter().any(|&v| v)
    }
    pub fn paint(&self, out: &mut [u32], width: u32, height: u32, scale: u32, damage: Rect) {
        let fill = |out: &mut [u32], r: Rect, c: u32| {
            policy::fill(out, width, height, scale, r, damage, rgba(c))
        };
        let tray = self.tray();
        rounded(out, width, height, scale, tray, damage, rgba(0x30363f), 12);
        rounded(
            out,
            width,
            height,
            scale,
            Rect::new(tray.x + 1, tray.y + 1, tray.w - 2, tray.h - 2),
            damage,
            rgba(0x22272f),
            11,
        );
        for group in self.model.groups() {
            let r = self.group_rect(&group.key);
            if r.empty() {
                continue;
            }
            if group.focused()
                || self.hover.as_ref() == Some(&group.key)
                || self.menu.as_ref() == Some(&group.key)
            {
                rounded(
                    out,
                    width,
                    height,
                    scale,
                    Rect::new(r.x + 2, r.y + 1, r.w - 4, 42),
                    damage,
                    rgba(if group.focused() { 0x3c4859 } else { 0x303843 }),
                    8,
                );
            }
            let tint = if group.all_minimized() {
                0x89929f
            } else {
                match group.icon.as_str() {
                    "terminal" => 0x8fc9b0,
                    "folder" => 0xd6bc89,
                    "calculator" => 0x98b9df,
                    "file-code" => 0xbcb1dc,
                    _ => 0xc2ccd8,
                }
            };
            icons::paint(
                out,
                width,
                height,
                scale,
                r.x + (r.w - 32) / 2,
                r.y + 3,
                &group.icon,
                damage,
                rgba(tint),
            );
            let dots = group.windows.len().min(3) as i32;
            for n in 0..dots {
                fill(
                    out,
                    Rect::new(r.x + r.w / 2 - (dots * 6 - 2) / 2 + n * 6, r.y + 43, 4, 3),
                    if group.focused() { 0x8db9ef } else { 0x6d7b8c },
                );
            }
        }
        if self.overflowing() {
            for (x, label, enabled) in [
                (tray.x + 4, "‹", self.offset > 0),
                (
                    tray.x + tray.w - 25,
                    "›",
                    self.offset + self.capacity() < self.model.groups().len(),
                ),
            ] {
                text::paint(
                    out,
                    width,
                    height,
                    scale,
                    Rect::new(x, tray.y + 17, 21, 20),
                    damage,
                    label,
                    rgba(if enabled { 0xd8dce3 } else { 0x525b68 }),
                    true,
                );
            }
        }
        if let Some(group) = self.menu_group() {
            let r = self.menu_rect();
            rounded(
                out,
                width,
                height,
                scale,
                Rect::new(r.x + 2, r.y + 3, r.w, r.h),
                damage,
                rgba(0x0b0e11),
                8,
            );
            rounded(out, width, height, scale, r, damage, rgba(0x4b5563), 8);
            rounded(
                out,
                width,
                height,
                scale,
                Rect::new(r.x + 1, r.y + 1, r.w - 2, r.h - 2),
                damage,
                rgba(0x252b33),
                7,
            );
            let count = group.windows.len();
            let title = if count > self.row_capacity() {
                format!(
                    "{}  ·  {}–{} / {}",
                    group.name,
                    self.scroll + 1,
                    (self.scroll + self.row_capacity()).min(count),
                    count
                )
            } else {
                format!(
                    "{}  ·  {} window{}",
                    group.name,
                    count,
                    if count == 1 { "" } else { "s" }
                )
            };
            text::paint(
                out,
                width,
                height,
                scale,
                Rect::new(r.x + 12, r.y + 7, r.w - 24, 18),
                damage,
                &title,
                rgba(0x9aa7b8),
                false,
            );
            for entry in self.entries() {
                if entry.index == self.selected && !entry.disabled {
                    rounded(
                        out,
                        width,
                        height,
                        scale,
                        entry.rect,
                        damage,
                        rgba(0x385b82),
                        4,
                    );
                }
                if entry.command == "minimize_all" {
                    fill(
                        out,
                        Rect::new(r.x + 10, entry.rect.y, r.w - 20, 1),
                        0x424b58,
                    );
                }
                let c = if entry.disabled {
                    0x697482
                } else if entry.command == "close_all" {
                    0xe2a3a3
                } else {
                    0xe0e5ec
                };
                if entry.window != 0 {
                    if let Some(window) = group.windows.iter().find(|w| w.id == entry.window) {
                        if window.focused {
                            text::paint(
                                out,
                                width,
                                height,
                                scale,
                                Rect::new(entry.rect.x + 5, entry.rect.y + 5, 15, 18),
                                damage,
                                "•",
                                rgba(0x9fc9f7),
                                true,
                            );
                        }
                    }
                }
                text::paint(
                    out,
                    width,
                    height,
                    scale,
                    Rect::new(entry.rect.x + 23, entry.rect.y + 5, entry.rect.w - 30, 18),
                    damage,
                    &entry.title,
                    rgba(c),
                    false,
                );
            }
        } else if let Some(group) = self.hover.as_ref().and_then(|k| self.model.group(k)) {
            let r = self.tooltip_rect();
            rounded(out, width, height, scale, r, damage, rgba(0x303843), 5);
            text::paint(
                out,
                width,
                height,
                scale,
                Rect::new(r.x + 8, r.y + 3, r.w - 16, 18),
                damage,
                &group.name,
                rgba(0xd8dce3),
                true,
            );
        }
    }
}
fn rgba(rgb: u32) -> u32 {
    ((rgb >> 16) & 255) | ((rgb >> 8) & 255) << 8 | (rgb & 255) << 16 | 0xff000000
}
fn rounded(
    out: &mut [u32],
    width: u32,
    height: u32,
    scale: u32,
    r: Rect,
    damage: Rect,
    c: u32,
    radius: i32,
) {
    let clip = r.intersect(damage);
    if clip.empty() {
        return;
    }
    for y in clip.y..clip.y + clip.h {
        let edge = (y - r.y).min(r.y + r.h - 1 - y);
        let inset = if edge < radius {
            let d = radius - edge;
            let mut x = 0;
            while (radius - x) * (radius - x) + d * d > radius * radius {
                x += 1;
            }
            x
        } else {
            0
        };
        policy::fill(
            out,
            width,
            height,
            scale,
            Rect::new(r.x + inset, y, r.w - inset * 2, 1),
            damage,
            c,
        );
    }
}
