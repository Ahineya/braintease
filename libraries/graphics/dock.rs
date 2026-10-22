//! Portable, window-driven application grouping for desktop docks.
//!
//! Input is a complete, revision-consistent session window snapshot. Processes
//! without an eligible window never create entries. Registered applications are
//! grouped by their declared application ID across instances; legacy windows
//! are grouped only by their lifetime-unique execution identity, never by a PID,
//! executable basename, or title. Direct-runtime windows without a trusted
//! execution identity retain separate window keys. Adapters read snapshots and
//! perform commands.

pub const VISIBLE: u16 = 1;
pub const MINIMIZED: u16 = 2;
pub const FOCUSED: u16 = 8;
pub const HIDDEN: u16 = 32;
pub const CLOSE_PENDING: u16 = 64;
pub const ROLE_NORMAL: u16 = 0;

/// Borrowed input, independent of the session model and any guest ABI.
#[derive(Clone, Copy, Debug, Default)]
pub struct WindowRecord<'a> {
    pub id: u32,
    pub process: u32,
    pub instance: u32,
    pub app_id: &'a str,
    pub name: &'a str,
    pub icon: &'a str,
    pub title: &'a str,
    pub role: u16,
    pub flags: u16,
    /// Dialogs and other owned child windows should set this to false.
    pub top_level: bool,
}

impl WindowRecord<'_> {
    fn eligible(self) -> bool {
        self.id != 0
            && self.top_level
            && self.role == ROLE_NORMAL
            && self.flags & HIDDEN == 0
            && self.flags & (VISIBLE | MINIMIZED) != 0
    }

    fn key(self) -> GroupKey {
        if self.process != 0 && self.instance != 0 && !self.app_id.is_empty() {
            GroupKey::Application(self.app_id.to_owned())
        } else if self.process == 0 {
            GroupKey::Window(self.id)
        } else {
            GroupKey::Process(self.process)
        }
    }

    fn matches_key(self, key: &GroupKey) -> bool {
        match key {
            GroupKey::Application(app_id) => {
                self.process != 0 && self.instance != 0 && self.app_id == app_id
            }
            GroupKey::Process(process) => {
                self.process != 0
                    && (self.instance == 0 || self.app_id.is_empty())
                    && self.process == *process
            }
            GroupKey::Window(id) => self.process == 0 && self.id == *id,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum GroupKey {
    Application(String),
    /// The session's execution identity, not the reusable numeric PID.
    Process(u32),
    /// Direct-runtime surface without a trusted process execution identity.
    Window(u32),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Window {
    pub id: u32,
    pub process: u32,
    pub instance: u32,
    pub title: String,
    pub minimized: bool,
    pub focused: bool,
    pub close_pending: bool,
}

impl From<WindowRecord<'_>> for Window {
    fn from(record: WindowRecord<'_>) -> Self {
        Self {
            id: record.id,
            process: record.process,
            instance: record.instance,
            title: record.title.to_owned(),
            minimized: record.flags & MINIMIZED != 0,
            focused: record.flags & FOCUSED != 0,
            close_pending: record.flags & CLOSE_PENDING != 0,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Group {
    pub key: GroupKey,
    pub name: String,
    pub icon: String,
    /// First-observed order, retained while each window stays eligible.
    pub windows: Vec<Window>,
}

impl Group {
    pub fn focused(&self) -> bool {
        self.windows.iter().any(|window| window.focused)
    }

    pub fn all_minimized(&self) -> bool {
        !self.windows.is_empty() && self.windows.iter().all(|window| window.minimized)
    }

    /// Multiple windows are deliberately exposed for user selection. Do not
    /// silently choose whichever window happened to be enumerated first.
    pub fn click(&self) -> Click {
        match self.windows.as_slice() {
            [window] => Click::Activate(window.id),
            _ => Click::Menu,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Click {
    Activate(u32),
    Menu,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Dock {
    groups: Vec<Group>,
}

impl Dock {
    pub fn new() -> Self {
        Self::default()
    }

    /// Group order remains stable while groups have eligible windows. New
    /// groups append in ascending first-window identity order. A group removed
    /// after its last window disappears rejoins at the end if launched again.
    pub fn groups(&self) -> &[Group] {
        &self.groups
    }

    pub fn group(&self, key: &GroupKey) -> Option<&Group> {
        self.groups.iter().find(|group| &group.key == key)
    }

    pub fn click(&self, key: &GroupKey) -> Option<Click> {
        self.group(key).map(Group::click)
    }

    /// Replace the complete snapshot. Returns whether user-visible dock state
    /// changed; enumeration order, geometry and unrelated process churn do not.
    /// Call only after a revision-consistent snapshot has completed successfully.
    pub fn synchronize(&mut self, records: &[WindowRecord<'_>]) -> bool {
        let mut records: Vec<_> = records
            .iter()
            .copied()
            .filter(|record| record.eligible())
            .collect();
        records.sort_unstable_by_key(|record| record.id);
        // Valid session snapshots have unique IDs. Be defensive if a future
        // adapter supplies duplicates, so menus never offer an identity twice.
        records.dedup_by_key(|record| record.id);
        let mut next: Vec<Group> = Vec::new();
        for &record in &records {
            let index = next.iter().position(|group| record.matches_key(&group.key));
            let index = index.unwrap_or_else(|| {
                next.push(Group {
                    key: record.key(),
                    name: String::new(),
                    icon: String::new(),
                    windows: Vec::new(),
                });
                next.len() - 1
            });
            next[index].windows.push(Window::from(record));
        }
        // The oldest current instance supplies metadata, regardless of input
        // enumeration order. Labels may update, but they never change the key.
        for group in &mut next {
            let canonical = records
                .iter()
                .filter(|record| record.matches_key(&group.key))
                .min_by_key(|record| (record.instance, record.id))
                .expect("groups are constructed from eligible windows");
            match &group.key {
                GroupKey::Application(app_id) => {
                    group.name = if canonical.name.is_empty() {
                        app_id.clone()
                    } else {
                        canonical.name.to_owned()
                    };
                    group.icon = canonical.icon.to_owned();
                }
                GroupKey::Process(_) | GroupKey::Window(_) => {
                    // A title is only a legacy display label, never an identity.
                    group.name = if canonical.title.is_empty() {
                        "Application".to_owned()
                    } else {
                        canonical.title.to_owned()
                    };
                }
            }
            if let Some(old) = self.group(&group.key) {
                group.windows.sort_by_key(|window| {
                    old.windows
                        .iter()
                        .position(|old_window| old_window.id == window.id)
                        .map_or((1, window.id as usize), |position| (0, position))
                });
            }
        }
        next.sort_by_key(|group| {
            self.groups
                .iter()
                .position(|old| old.key == group.key)
                .map_or((1, group.windows[0].id as usize), |position| (0, position))
        });
        if self.groups == next {
            false
        } else {
            self.groups = next;
            true
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn window(id: u32, process: u32, instance: u32, app_id: &str) -> WindowRecord<'_> {
        WindowRecord {
            id,
            process,
            instance,
            app_id,
            name: app_id,
            icon: "application-icon",
            title: "Untitled",
            role: ROLE_NORMAL,
            flags: VISIBLE,
            top_level: true,
        }
    }

    fn keys(dock: &Dock) -> Vec<GroupKey> {
        dock.groups()
            .iter()
            .map(|group| group.key.clone())
            .collect()
    }

    fn ids(group: &Group) -> Vec<u32> {
        group.windows.iter().map(|window| window.id).collect()
    }

    #[test]
    fn application_merges_independent_instances_and_explicit_helper_windows() {
        let mut dock = Dock::new();
        dock.synchronize(&[
            window(20, 200, 2, "org.riplix.Rift"),
            window(10, 100, 1, "org.riplix.Rift"),
            window(30, 300, 1, "org.riplix.Rift"),
        ]);
        assert_eq!(dock.groups().len(), 1);
        let group = &dock.groups()[0];
        assert_eq!(group.key, GroupKey::Application("org.riplix.Rift".into()));
        assert_eq!(ids(group), vec![10, 20, 30]);
        assert_eq!(group.windows[1].instance, 2);
        assert_eq!(group.windows[2].process, 300);
        assert_eq!(group.click(), Click::Menu);
    }

    #[test]
    fn process_without_window_never_creates_a_dock_entry() {
        let mut dock = Dock::new();
        assert!(!dock.synchronize(&[]));
        // The registry can contain any number of registered apps and helper
        // processes; the model has no process-list input to mistake for windows.
        dock.synchronize(&[window(1, 10, 1, "rift")]);
        assert_eq!(dock.groups().len(), 1);
        assert_eq!(dock.groups()[0].windows.len(), 1);
        assert!(dock.synchronize(&[]));
        assert!(dock.groups().is_empty());
    }

    #[test]
    fn hidden_unmapped_child_and_special_role_windows_are_excluded() {
        let mut hidden = window(1, 10, 1, "rift");
        hidden.flags |= HIDDEN;
        let mut unmapped = window(2, 10, 1, "rift");
        unmapped.flags = 0;
        let mut child = window(3, 10, 1, "rift");
        child.top_level = false;
        let mut panel = window(4, 20, 2, "dock");
        panel.role = 1;
        let no_identity = window(0, 10, 1, "rift");
        let mut dock = Dock::new();
        assert!(!dock.synchronize(&[hidden, unmapped, child, panel, no_identity]));
        hidden.flags = HIDDEN | MINIMIZED;
        assert!(!dock.synchronize(&[hidden]));
    }

    #[test]
    fn minimized_windows_remain_and_single_click_requests_activation() {
        let mut minimized = window(10, 100, 1, "rift");
        minimized.flags = MINIMIZED | CLOSE_PENDING;
        let mut dock = Dock::new();
        dock.synchronize(&[minimized]);
        let group = &dock.groups()[0];
        assert!(group.all_minimized());
        assert!(!group.focused());
        assert!(group.windows[0].close_pending);
        assert_eq!(dock.click(&group.key), Some(Click::Activate(10)));
        minimized.flags = VISIBLE | FOCUSED;
        assert!(dock.synchronize(&[minimized]));
        assert!(!dock.groups()[0].all_minimized());
        assert!(dock.groups()[0].focused());
        assert!(!dock.groups()[0].windows[0].close_pending);
    }

    #[test]
    fn legacy_identity_uses_execution_not_matching_titles_or_program_names() {
        let mut dock = Dock::new();
        let mut legacy = window(10, 100, 0, "ignored-without-instance");
        legacy.title = "Rift";
        let mut another = legacy;
        another.id = 20;
        another.process = 200;
        let mut same_execution = legacy;
        same_execution.id = 30;
        dock.synchronize(&[legacy, another, same_execution]);
        assert_eq!(
            keys(&dock),
            vec![GroupKey::Process(100), GroupKey::Process(200)]
        );
        assert_eq!(ids(&dock.groups()[0]), vec![10, 30]);
        assert_eq!(ids(&dock.groups()[1]), vec![20]);
        // A reused numeric PID is intentionally not representable here. A new
        // execution identity always creates its own group.
        legacy.process = 300;
        dock.synchronize(&[legacy]);
        assert_eq!(keys(&dock), vec![GroupKey::Process(300)]);
    }

    #[test]
    fn title_churn_updates_labels_without_duplicate_window_or_group_identity() {
        let mut record = window(10, 100, 1, "rift");
        let mut dock = Dock::new();
        dock.synchronize(&[record]);
        let key = dock.groups()[0].key.clone();
        record.title = "notes.txt — Rift";
        assert!(dock.synchronize(&[record]));
        assert_eq!(keys(&dock), vec![key]);
        assert_eq!(ids(&dock.groups()[0]), vec![10]);
        assert_eq!(dock.groups()[0].name, "rift");
        assert_eq!(dock.groups()[0].windows[0].title, record.title);
        assert!(!dock.synchronize(&[record]));
        record.instance = 0;
        dock.synchronize(&[record]);
        assert_eq!(dock.groups()[0].key, GroupKey::Process(100));
        assert_eq!(dock.groups()[0].name, record.title);
    }

    #[test]
    fn direct_runtime_surfaces_have_distinct_window_keys_without_forged_grouping() {
        let a = window(10, 0, 1, "pretend-app");
        let b = window(20, 0, 1, "pretend-app");
        let c = window(30, 10, 0, "");
        let mut dock = Dock::new();
        dock.synchronize(&[b, c, a]);
        assert_eq!(
            keys(&dock),
            vec![
                GroupKey::Window(10),
                GroupKey::Window(20),
                GroupKey::Process(10)
            ]
        );
        assert_eq!(dock.groups()[0].name, "Untitled");
        assert_eq!(dock.click(&GroupKey::Window(10)), Some(Click::Activate(10)));
        assert_eq!(
            dock.click(&GroupKey::Process(10)),
            Some(Click::Activate(30))
        );
        assert!(!dock.synchronize(&[a, b, c]));
    }

    #[test]
    fn surviving_groups_and_windows_keep_order_while_new_ones_append() {
        let a = window(20, 100, 1, "a");
        let b = window(30, 200, 2, "b");
        let c = window(10, 300, 3, "c");
        let a_earlier = window(5, 100, 1, "a");
        let mut dock = Dock::new();
        dock.synchronize(&[b, a]);
        assert!(!dock.synchronize(&[a, b]));
        dock.synchronize(&[c, b, a_earlier, a]);
        assert_eq!(
            keys(&dock),
            vec![
                GroupKey::Application("a".into()),
                GroupKey::Application("b".into()),
                GroupKey::Application("c".into()),
            ]
        );
        assert_eq!(ids(&dock.groups()[0]), vec![20, 5]);
        dock.synchronize(&[a_earlier, c]);
        assert_eq!(ids(&dock.groups()[0]), vec![5]);
        dock.synchronize(&[b, c, a_earlier]);
        assert_eq!(
            keys(&dock),
            vec![
                GroupKey::Application("a".into()),
                GroupKey::Application("c".into()),
                GroupKey::Application("b".into()),
            ]
        );
    }

    #[test]
    fn metadata_is_deterministic_and_updates_without_moving_the_group() {
        let mut first = window(30, 100, 1, "rift");
        first.name = "Rift";
        first.icon = "rift.svg";
        let mut second = window(10, 200, 2, "rift");
        second.name = "Different instance label";
        second.icon = "other.svg";
        let mut dock = Dock::new();
        dock.synchronize(&[second, first]);
        assert_eq!(dock.groups()[0].name, "Rift");
        assert_eq!(dock.groups()[0].icon, "rift.svg");
        assert!(!dock.synchronize(&[first, second]));
        first.name = "Рифт";
        first.icon = "updated.svg";
        assert!(dock.synchronize(&[second, first]));
        assert_eq!(dock.groups()[0].name, "Рифт");
        assert_eq!(dock.groups()[0].icon, "updated.svg");
        dock.synchronize(&[second]);
        assert_eq!(dock.groups()[0].name, second.name);
        second.name = "";
        dock.synchronize(&[second]);
        assert_eq!(dock.groups()[0].name, "rift");
    }

    #[test]
    fn large_window_lists_have_no_artificial_application_limit() {
        let records: Vec<_> = (1..=300).map(|id| window(id, id, 0, "")).collect();
        let mut dock = Dock::new();
        dock.synchronize(&records);
        assert_eq!(dock.groups().len(), 300);
        let mut reversed = records;
        reversed.reverse();
        assert!(!dock.synchronize(&reversed));
        assert!(dock.click(&GroupKey::Process(999)).is_none());
    }
}
