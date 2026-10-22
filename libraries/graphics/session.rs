//! Portable desktop session identities and revisioned window snapshots.
//!
//! The adapter supplies trusted, lifetime-unique process and window identities.
//! Numeric PIDs are display metadata and must never be used as identities. The
//! adapter also authenticates callers and parent/child relationships; this model
//! deliberately knows nothing about host processes, guest memory, or authority.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Invalid,
    NotFound,
    Again,
    Exhausted,
    Capacity,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Process {
    pub id: u32,
    pub pid: u16,
    /// Zero means that the process has not joined an application instance.
    pub instance: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Instance {
    pub id: u32,
    /// Zero after the original owner exits while helper processes remain.
    pub owner: u32,
    pub app_id: String,
    pub name: String,
    pub icon: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Window {
    pub id: u32,
    pub process: u32,
    /// Derived from `process`; caller-supplied values are ignored on sync.
    pub instance: u32,
    pub title: String,
    pub x: i32,
    pub y: i32,
    pub width: u16,
    pub height: u16,
    pub flags: u16,
    pub role: u16,
    pub workspace: u32,
}

#[derive(Debug)]
pub struct Registry {
    revision: u32,
    last_instance: u32,
    processes: Vec<Process>,
    instances: Vec<Instance>,
    windows: Vec<Window>,
}

impl Default for Registry {
    fn default() -> Self {
        Self::new()
    }
}

impl Registry {
    pub fn new() -> Self {
        Self {
            revision: 1,
            last_instance: 0,
            processes: Vec::new(),
            instances: Vec::new(),
            windows: Vec::new(),
        }
    }

    /// Obtain this value before enumerating; pass it with each cursor request.
    pub fn revision(&self) -> u32 {
        self.revision
    }

    /// Reject continuation of a snapshot that changed between requests.
    pub fn check_revision(&self, revision: u32) -> Result<(), Error> {
        if revision == self.revision {
            Ok(())
        } else {
            Err(Error::Again)
        }
    }

    /// Iterators always enumerate in ascending lifetime-unique identity order.
    pub fn processes(&self) -> impl Iterator<Item = &Process> {
        self.processes.iter()
    }

    pub fn instances(&self) -> impl Iterator<Item = &Instance> {
        self.instances.iter()
    }

    pub fn windows(&self) -> impl Iterator<Item = &Window> {
        self.windows.iter()
    }

    pub fn process(&self, id: u32) -> Option<&Process> {
        self.process_index(id).ok().map(|i| &self.processes[i])
    }

    pub fn instance(&self, id: u32) -> Option<&Instance> {
        self.instance_index(id).ok().map(|i| &self.instances[i])
    }

    pub fn window(&self, id: u32) -> Option<&Window> {
        self.windows
            .binary_search_by_key(&id, |window| window.id)
            .ok()
            .map(|i| &self.windows[i])
    }

    /// Cursor zero starts enumeration. The returned object's ID is the next
    /// cursor; `None` marks the end, including when the cursor was removed.
    pub fn next_process(&self, revision: u32, after: u32) -> Result<Option<&Process>, Error> {
        self.check_revision(revision)?;
        Ok(self
            .processes
            .get(self.processes.partition_point(|p| p.id <= after)))
    }

    pub fn next_instance(&self, revision: u32, after: u32) -> Result<Option<&Instance>, Error> {
        self.check_revision(revision)?;
        Ok(self
            .instances
            .get(self.instances.partition_point(|p| p.id <= after)))
    }

    pub fn next_window(&self, revision: u32, after: u32) -> Result<Option<&Window>, Error> {
        self.check_revision(revision)?;
        Ok(self
            .windows
            .get(self.windows.partition_point(|p| p.id <= after)))
    }

    /// Track a process before it registers or creates a window. Repeated calls
    /// with unchanged metadata leave the snapshot revision unchanged.
    pub fn touch_process(&mut self, id: u32, pid: u16) -> Result<(), Error> {
        if id == 0 {
            return Err(Error::Invalid);
        }
        match self.process_index(id) {
            Ok(index) => {
                if self.processes[index].pid != pid {
                    let revision = self.changed_revision()?;
                    self.processes[index].pid = pid;
                    self.revision = revision;
                }
            }
            Err(index) => {
                let revision = self.changed_revision()?;
                self.processes.try_reserve(1).map_err(|_| Error::Capacity)?;
                self.processes.insert(
                    index,
                    Process {
                        id,
                        pid,
                        instance: 0,
                    },
                );
                self.revision = revision;
            }
        }
        Ok(())
    }

    /// Register the calling process, or let an instance owner update its labels.
    /// Each independent registration creates an instance even for equal app IDs.
    /// An inherited helper remains in its parent's instance and may repeat its
    /// unchanged metadata. Application IDs cannot change while an instance lives.
    pub fn register(
        &mut self,
        id: u32,
        pid: u16,
        app_id: &str,
        name: &str,
        icon: &str,
    ) -> Result<u32, Error> {
        if id == 0 || !valid_metadata(app_id, name, icon) {
            return Err(Error::Invalid);
        }
        let process_index = self.process_index(id);
        if let Ok(index) = process_index {
            let instance_id = self.processes[index].instance;
            if instance_id != 0 {
                let ii = self
                    .instance_index(instance_id)
                    .map_err(|_| Error::Invalid)?;
                let instance = &self.instances[ii];
                if instance.app_id != app_id {
                    return Err(Error::Invalid);
                }
                if instance.owner != id && (instance.name != name || instance.icon != icon) {
                    return Err(Error::Invalid);
                }
                if self.processes[index].pid == pid
                    && instance.name == name
                    && instance.icon == icon
                {
                    return Ok(instance_id);
                }
                let revision = self.changed_revision()?;
                let name = checked_string(name)?;
                let icon = checked_string(icon)?;
                self.processes[index].pid = pid;
                self.instances[ii].name = name;
                self.instances[ii].icon = icon;
                self.revision = revision;
                return Ok(instance_id);
            }
        }
        let instance_id = self.last_instance.checked_add(1).ok_or(Error::Exhausted)?;
        let revision = self.changed_revision()?;
        let instance = Instance {
            id: instance_id,
            owner: id,
            app_id: checked_string(app_id)?,
            name: checked_string(name)?,
            icon: checked_string(icon)?,
        };
        self.instances.try_reserve(1).map_err(|_| Error::Capacity)?;
        if process_index.is_err() {
            self.processes.try_reserve(1).map_err(|_| Error::Capacity)?;
        }
        match process_index {
            Ok(index) => {
                self.processes[index].pid = pid;
                self.processes[index].instance = instance_id;
            }
            Err(index) => self.processes.insert(
                index,
                Process {
                    id,
                    pid,
                    instance: instance_id,
                },
            ),
        }
        for window in &mut self.windows {
            if window.process == id {
                window.instance = instance_id;
            }
        }
        self.instances.push(instance);
        self.last_instance = instance_id;
        self.revision = revision;
        Ok(instance_id)
    }

    /// Associate an authenticated child with the parent's application. This does
    /// not perform parent/child authentication; the operating-system adapter must
    /// check that relationship before calling. Existing instances cannot be stolen.
    pub fn attach_child(&mut self, parent: u32, child: u32, child_pid: u16) -> Result<u32, Error> {
        if parent == 0 || child == 0 || parent == child {
            return Err(Error::Invalid);
        }
        let instance = self.process(parent).ok_or(Error::NotFound)?.instance;
        if instance == 0 {
            return Err(Error::Invalid);
        }
        let index = self.process_index(child);
        if let Ok(index) = index {
            let process = &self.processes[index];
            if process.instance != 0 && process.instance != instance {
                return Err(Error::Invalid);
            }
            if process.instance == instance && process.pid == child_pid {
                return Ok(instance);
            }
        }
        let revision = self.changed_revision()?;
        if index.is_err() {
            self.processes.try_reserve(1).map_err(|_| Error::Capacity)?;
        }
        match index {
            Ok(index) => {
                self.processes[index].pid = child_pid;
                self.processes[index].instance = instance;
            }
            Err(index) => self.processes.insert(
                index,
                Process {
                    id: child,
                    pid: child_pid,
                    instance,
                },
            ),
        }
        for window in &mut self.windows {
            if window.process == child {
                window.instance = instance;
            }
        }
        self.revision = revision;
        Ok(instance)
    }

    /// Remove one process and its windows. The instance survives its owner while
    /// helpers remain, and disappears only after its last member exits. Missing
    /// processes are an idempotent no-op.
    pub fn remove_process(&mut self, id: u32) -> Result<bool, Error> {
        let Ok(index) = self.process_index(id) else {
            return Ok(false);
        };
        let instance = self.processes[index].instance;
        let instance_index = if instance == 0 {
            None
        } else {
            Some(self.instance_index(instance).map_err(|_| Error::Invalid)?)
        };
        let revision = self.changed_revision()?;
        self.processes.remove(index);
        self.windows.retain(|window| window.process != id);
        if let Some(ii) = instance_index {
            if self.processes().any(|process| process.instance == instance) {
                if self.instances[ii].owner == id {
                    self.instances[ii].owner = 0;
                }
            } else {
                self.instances.remove(ii);
            }
        }
        self.revision = revision;
        Ok(true)
    }

    /// Replace the complete window snapshot in one revision. Order and supplied
    /// instance fields are irrelevant; only known processes and distinct nonzero
    /// window identities are accepted. An invalid snapshot changes nothing.
    pub fn synchronize_windows(&mut self, mut windows: Vec<Window>) -> Result<bool, Error> {
        windows.sort_unstable_by_key(|window| window.id);
        let mut previous = 0;
        for window in &mut windows {
            if window.id == 0 || window.id == previous {
                return Err(Error::Invalid);
            }
            previous = window.id;
            window.instance = self
                .process(window.process)
                .ok_or(Error::NotFound)?
                .instance;
        }
        if self.windows == windows {
            return Ok(false);
        }
        let revision = self.changed_revision()?;
        self.windows = windows;
        self.revision = revision;
        Ok(true)
    }

    fn process_index(&self, id: u32) -> Result<usize, usize> {
        self.processes
            .binary_search_by_key(&id, |process| process.id)
    }

    fn instance_index(&self, id: u32) -> Result<usize, usize> {
        self.instances
            .binary_search_by_key(&id, |instance| instance.id)
    }

    fn changed_revision(&self) -> Result<u32, Error> {
        self.revision.checked_add(1).ok_or(Error::Exhausted)
    }
}

fn valid_metadata(app_id: &str, name: &str, icon: &str) -> bool {
    !app_id.is_empty()
        && app_id.len() <= 63
        && app_id
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || b"._-".contains(&byte))
        && name.len() <= 63
        && icon.len() <= 127
}

fn checked_string(text: &str) -> Result<String, Error> {
    let mut string = String::new();
    string
        .try_reserve_exact(text.len())
        .map_err(|_| Error::Capacity)?;
    string.push_str(text);
    Ok(string)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn window(id: u32, process: u32) -> Window {
        Window {
            id,
            process,
            instance: 999,
            title: format!("Window {id}"),
            x: 10,
            y: 20,
            width: 640,
            height: 480,
            flags: 0,
            role: 0,
            workspace: 1,
        }
    }

    #[test]
    fn pid_reuse_has_distinct_identity_and_stale_handles_disappear() {
        let mut registry = Registry::new();
        let first = registry.register(10, 7, "rift", "Rift", "rift").unwrap();
        registry.synchronize_windows(vec![window(20, 10)]).unwrap();
        registry.remove_process(10).unwrap();
        assert!(registry.process(10).is_none());
        assert!(registry.window(20).is_none());
        assert!(registry.instance(first).is_none());
        let second = registry.register(11, 7, "rift", "Rift", "rift").unwrap();
        assert!(second > first);
        assert_eq!(registry.process(11).unwrap().pid, 7);
        assert_eq!(registry.process(11).unwrap().instance, second);
    }

    #[test]
    fn metadata_updates_and_no_ops_have_precise_revisions() {
        let mut registry = Registry::new();
        registry.touch_process(10, 1).unwrap();
        let touched = registry.revision();
        registry.touch_process(10, 1).unwrap();
        assert_eq!(registry.revision(), touched);
        let instance = registry
            .register(10, 1, "org.riplix.Rift", "Rift", "rift")
            .unwrap();
        let registered = registry.revision();
        assert_eq!(registered, touched + 1);
        assert_eq!(
            registry.register(10, 1, "org.riplix.Rift", "Rift", "rift"),
            Ok(instance)
        );
        assert_eq!(registry.revision(), registered);
        registry
            .register(10, 2, "org.riplix.Rift", "Рифт", "new-icon")
            .unwrap();
        assert_eq!(registry.revision(), registered + 1);
        assert_eq!(registry.instance(instance).unwrap().name, "Рифт");
        assert_eq!(registry.process(10).unwrap().pid, 2);
        assert_eq!(
            registry.register(10, 2, "other", "Other", ""),
            Err(Error::Invalid)
        );
        assert_eq!(registry.revision(), registered + 1);
        assert!(!registry.remove_process(99).unwrap());
        assert_eq!(registry.revision(), registered + 1);
    }

    #[test]
    fn independent_launches_and_explicit_helpers() {
        let mut registry = Registry::new();
        let first = registry.register(10, 1, "game", "Game", "").unwrap();
        let second = registry.register(20, 2, "game", "Game", "").unwrap();
        assert_ne!(first, second);
        registry.touch_process(30, 3).unwrap();
        registry.synchronize_windows(vec![window(1, 30)]).unwrap();
        let before = registry.revision();
        assert_eq!(registry.attach_child(10, 30, 3), Ok(first));
        assert_eq!(registry.revision(), before + 1);
        assert_eq!(registry.window(1).unwrap().instance, first);
        assert_eq!(registry.attach_child(10, 30, 3), Ok(first));
        assert_eq!(registry.revision(), before + 1);
        assert_eq!(registry.attach_child(20, 30, 3), Err(Error::Invalid));
        assert_eq!(
            registry.register(30, 3, "different", "Helper", ""),
            Err(Error::Invalid)
        );
        assert_eq!(registry.register(30, 3, "game", "Game", ""), Ok(first));
        assert_eq!(registry.instances().count(), 2);
    }

    #[test]
    fn orphan_helpers_retain_instance_until_last_exit() {
        let mut registry = Registry::new();
        let instance = registry.register(10, 1, "game", "Game", "").unwrap();
        registry.attach_child(10, 20, 2).unwrap();
        registry.attach_child(20, 30, 3).unwrap();
        registry
            .synchronize_windows(vec![window(1, 10), window(2, 20)])
            .unwrap();
        registry.remove_process(10).unwrap();
        assert_eq!(registry.instance(instance).unwrap().owner, 0);
        assert!(registry.window(1).is_none());
        assert_eq!(registry.window(2).unwrap().instance, instance);
        registry.remove_process(20).unwrap();
        assert!(registry.instance(instance).is_some());
        registry.remove_process(30).unwrap();
        assert!(registry.instance(instance).is_none());
    }

    #[test]
    fn snapshots_are_ordered_revision_checked_and_coalesced() {
        let mut registry = Registry::new();
        registry.touch_process(30, 3).unwrap();
        registry.touch_process(10, 1).unwrap();
        registry
            .synchronize_windows(vec![window(20, 30), window(10, 10)])
            .unwrap();
        let revision = registry.revision();
        assert_eq!(
            registry.processes().map(|p| p.id).collect::<Vec<_>>(),
            vec![10, 30]
        );
        assert_eq!(registry.next_process(revision, 0).unwrap().unwrap().id, 10);
        assert_eq!(registry.next_process(revision, 10).unwrap().unwrap().id, 30);
        assert!(registry.next_process(revision, 30).unwrap().is_none());
        assert_eq!(registry.next_window(revision, 0).unwrap().unwrap().id, 10);
        assert_eq!(registry.window(10).unwrap().instance, 0);
        assert!(!registry
            .synchronize_windows(vec![window(20, 30), window(10, 10)])
            .unwrap());
        assert_eq!(registry.revision(), revision);
        let mut changed = vec![window(10, 10), window(20, 30)];
        changed[0].x += 1;
        changed[1].title = "Updated".to_string();
        registry.synchronize_windows(changed).unwrap();
        assert_eq!(registry.revision(), revision + 1);
        assert_eq!(registry.next_window(revision, 0), Err(Error::Again));
        assert_eq!(registry.next_process(revision, 0), Err(Error::Again));
        assert_eq!(registry.next_instance(revision, 0), Err(Error::Again));
    }

    #[test]
    fn registration_updates_existing_window_group_in_one_revision() {
        let mut registry = Registry::new();
        registry.touch_process(10, 1).unwrap();
        registry.synchronize_windows(vec![window(1, 10)]).unwrap();
        let before = registry.revision();
        let instance = registry.register(10, 1, "app", "App", "").unwrap();
        assert_eq!(registry.window(1).unwrap().instance, instance);
        assert_eq!(registry.revision(), before + 1);
    }

    #[test]
    fn invalid_metadata_and_snapshots_are_atomic() {
        let mut registry = Registry::new();
        let invalid_ids = ["", "contains space", "slash/app", "é", &"a".repeat(64)];
        for app_id in invalid_ids {
            assert_eq!(
                registry.register(1, 1, app_id, "App", ""),
                Err(Error::Invalid)
            );
        }
        assert_eq!(
            registry.register(0, 1, "app", "App", ""),
            Err(Error::Invalid)
        );
        assert_eq!(
            registry.register(1, 1, "app", &"é".repeat(32), ""),
            Err(Error::Invalid)
        );
        assert_eq!(
            registry.register(1, 1, "app", "App", &"i".repeat(128)),
            Err(Error::Invalid)
        );
        assert_eq!(registry.revision(), 1);
        assert_eq!(registry.processes().count(), 0);
        registry.touch_process(10, 1).unwrap();
        registry.synchronize_windows(vec![window(1, 10)]).unwrap();
        let before = registry.revision();
        assert_eq!(
            registry.synchronize_windows(vec![window(1, 10), window(1, 10)]),
            Err(Error::Invalid)
        );
        assert_eq!(
            registry.synchronize_windows(vec![window(2, 99)]),
            Err(Error::NotFound)
        );
        assert_eq!(
            registry.synchronize_windows(vec![window(0, 10)]),
            Err(Error::Invalid)
        );
        assert_eq!(registry.revision(), before);
        assert_eq!(registry.windows().count(), 1);
        assert!(registry.window(1).is_some());
    }

    #[test]
    fn dynamic_registry_has_no_small_application_limit() {
        let mut registry = Registry::new();
        for id in (1..=300).rev() {
            registry
                .register(id, id as u16, "app", "Application", "")
                .unwrap();
        }
        assert_eq!(registry.processes().count(), 300);
        assert_eq!(registry.instances().count(), 300);
        assert_eq!(
            registry.processes().map(|p| p.id).collect::<Vec<_>>(),
            (1..=300).collect::<Vec<_>>()
        );
        assert_eq!(
            registry.instances().map(|p| p.id).collect::<Vec<_>>(),
            (1..=300).collect::<Vec<_>>()
        );
    }

    #[test]
    fn identity_and_revision_exhaustion_never_wrap_or_partially_mutate() {
        let mut registry = Registry::new();
        registry.last_instance = u32::MAX - 1;
        assert_eq!(registry.register(1, 1, "app", "App", ""), Ok(u32::MAX));
        let before = registry.revision();
        assert_eq!(
            registry.register(2, 2, "app", "App", ""),
            Err(Error::Exhausted)
        );
        assert_eq!(registry.revision(), before);
        assert!(registry.process(2).is_none());
        registry.revision = u32::MAX;
        assert_eq!(registry.touch_process(3, 3), Err(Error::Exhausted));
        assert_eq!(registry.touch_process(1, 2), Err(Error::Exhausted));
        assert_eq!(registry.register(1, 1, "app", "App", ""), Ok(u32::MAX));
        assert_eq!(
            registry.register(1, 1, "app", "Changed", ""),
            Err(Error::Exhausted)
        );
        assert_eq!(registry.attach_child(1, 2, 2), Err(Error::Exhausted));
        assert_eq!(registry.remove_process(1), Err(Error::Exhausted));
        assert_eq!(
            registry.synchronize_windows(vec![window(1, 1)]),
            Err(Error::Exhausted)
        );
        assert_eq!(registry.revision(), u32::MAX);
        assert_eq!(registry.process(1).unwrap().pid, 1);
        assert_eq!(registry.instance(u32::MAX).unwrap().name, "App");
        assert_eq!(registry.processes().count(), 1);
        assert_eq!(registry.windows().count(), 0);
    }
    #[test]
    fn trusted_panel_promotion_changes_snapshot_role_once() {
        let mut registry = Registry::new();
        registry.touch_process(10, 42).unwrap();
        registry.synchronize_windows(vec![window(1, 10)]).unwrap();
        let revision = registry.revision();
        let mut panel = window(1, 10);
        panel.role = 1;
        registry.synchronize_windows(vec![panel.clone()]).unwrap();
        assert_eq!(registry.window(1).unwrap().role, 1);
        assert_eq!(registry.revision(), revision + 1);
        registry.synchronize_windows(vec![panel]).unwrap();
        assert_eq!(registry.revision(), revision + 1);
        assert_eq!(registry.check_revision(revision), Err(Error::Again));
    }

}
