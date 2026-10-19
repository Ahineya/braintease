//! Directory-capability bridge for the hosted kernel. No host FD enters guest memory.
#[cfg(unix)]
mod unix {
    use crate::{memory::linear, Runtime, CELLS};
    use std::{
        collections::BTreeMap,
        ffi::{CStr, CString},
        fs::{File, OpenOptions},
        io,
        os::{
            fd::{AsRawFd, FromRawFd},
            unix::{
                ffi::OsStrExt,
                fs::{MetadataExt, OpenOptionsExt},
            },
        },
        sync::Arc,
    };

    type Identity = (u64, u64);
    type Result<T> = std::result::Result<T, u16>;
    struct Node {
        file: Arc<File>,
        parent: Arc<File>,
        name: CString,
        writer: Option<Arc<File>>,
        entries: Option<Vec<u16>>,
        directory: bool,
    }
    #[derive(Default)]
    pub(crate) struct HostFs {
        root: Option<Arc<File>>,
        identities: BTreeMap<Identity, u16>,
        nodes: BTreeMap<u16, Node>,
        dirty: BTreeMap<u16, Arc<File>>,
    }
    fn error(e: io::Error) -> u16 {
        match e.raw_os_error().unwrap_or(libc::EIO) {
            libc::EPERM => 1,
            libc::ENOENT => 2,
            libc::EINTR => 4,
            libc::EBADF => 9,
            libc::EAGAIN => 11,
            libc::ENOMEM => 12,
            libc::EACCES | libc::ELOOP => 13,
            libc::EBUSY => 16,
            libc::EEXIST => 17,
            libc::EXDEV => 18,
            libc::ENOTDIR => 20,
            libc::EISDIR => 21,
            libc::EINVAL => 22,
            libc::ENFILE => 23,
            libc::EMFILE => 24,
            libc::EFBIG | libc::EOVERFLOW => 27,
            libc::ENOSPC => 28,
            libc::EROFS => 30,
            libc::EMLINK => 31,
            libc::ENOTEMPTY => 36,
            libc::ENAMETOOLONG => 37,
            _ => 5,
        }
    }
    fn checked(value: libc::c_int) -> Result<libc::c_int> {
        if value < 0 {
            Err(error(io::Error::last_os_error()))
        } else {
            Ok(value)
        }
    }
    fn ident(file: &File) -> Result<Identity> {
        let m = file.metadata().map_err(error)?;
        Ok((m.dev(), m.ino()))
    }
    fn open_at(parent: &File, name: &CStr, flags: libc::c_int, mode: u32) -> Result<File> {
        let fd = checked(unsafe {
            libc::openat(
                parent.as_raw_fd(),
                name.as_ptr(),
                flags | libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK,
                mode as libc::c_uint,
            )
        })?;
        let file = unsafe { File::from_raw_fd(fd) };
        let m = file.metadata().map_err(error)?;
        if !m.is_file() && !m.is_dir() {
            return Err(44);
        }
        Ok(file)
    }
    fn component(bytes: Vec<u8>) -> Result<CString> {
        if bytes.is_empty() || bytes.contains(&b'/') || bytes == b"." || bytes == b".." {
            return Err(22);
        }
        if bytes.len() > 30 {
            return Err(37);
        }
        CString::new(bytes).map_err(|_| 22)
    }
    impl HostFs {
        fn id(&mut self, identity: Identity) -> Result<u16> {
            if let Some(&id) = self.identities.get(&identity) {
                return Ok(id);
            }
            let id = u16::try_from(self.identities.len() + 1).map_err(|_| 23u16)?;
            self.identities.insert(identity, id);
            Ok(id)
        }
        fn insert(&mut self, file: File, parent: Arc<File>, name: CString) -> Result<u16> {
            let metadata = file.metadata().map_err(error)?;
            let id = self.id((metadata.dev(), metadata.ino()))?;
            self.nodes
                .entry(id)
                .and_modify(|n| {
                    if id != 1 {
                        n.parent = parent.clone();
                        n.name = name.clone();
                    }
                })
                .or_insert(Node {
                    file: Arc::new(file),
                    parent,
                    name,
                    writer: None,
                    entries: None,
                    directory: metadata.is_dir(),
                });
            Ok(id)
        }
        fn file(&self, id: u16) -> Result<Arc<File>> {
            self.nodes.get(&id).map(|n| n.file.clone()).ok_or(9)
        }
        // Directory operations must remain below the chosen root even if a
        // Mac process has moved an open directory outside the exported tree.
        fn directory(&self, id: u16) -> Result<Arc<File>> {
            let file = self.file(id)?;
            if !self.nodes[&id].directory {
                return Err(20);
            }
            self.beneath(&file)?;
            Ok(file)
        }
        fn beneath(&self, file: &Arc<File>) -> Result<()> {
            let root = ident(self.root.as_ref().ok_or(19u16)?)?;
            let mut at = file.clone();
            for _ in 0..256 {
                let current = ident(&at)?;
                if current == root {
                    return Ok(());
                }
                let parent = Arc::new(open_at(&at, c"..", libc::O_RDONLY | libc::O_DIRECTORY, 0)?);
                if ident(&parent)? == current {
                    break;
                }
                at = parent;
            }
            Err(13)
        }
        fn mount(&mut self, bytes: Vec<u8>) -> Result<u16> {
            if self.root.is_some() {
                return Err(16);
            }
            let path = std::ffi::OsStr::from_bytes(&bytes);
            let file = OpenOptions::new()
                .read(true)
                .custom_flags(libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC)
                .open(path)
                .map_err(error)?;
            let parent = Arc::new(file.try_clone().map_err(error)?);
            self.root = Some(parent.clone());
            self.insert(file, parent, CString::new("").unwrap())
        }
        fn lookup(&mut self, parent: u16, bytes: Vec<u8>) -> Result<u16> {
            let dir = self.directory(parent)?;
            if bytes == b"." || (bytes == b".." && parent == 1) {
                return Ok(parent);
            }
            let name = if bytes == b".." {
                CString::new("..").unwrap()
            } else {
                component(bytes)?
            };
            let file = open_at(&dir, &name, libc::O_RDONLY, 0)?;
            // .. is checked before publishing any reference to the parent.
            if name.as_bytes() == b".." {
                self.beneath(&Arc::new(file.try_clone().map_err(error)?))?;
            }
            self.insert(file, dir, name)
        }
        fn writable(&mut self, id: u16) -> Result<Arc<File>> {
            let n = self.nodes.get(&id).ok_or(9u16)?;
            // An opened object's kind cannot change in place. Reusing its
            // established writer needs neither stat nor another path lookup.
            if let Some(writer) = &n.writer {
                return Ok(writer.clone());
            }
            if n.directory {
                return Err(21);
            }
            if n.writer.is_none() {
                self.beneath(&n.parent)?;
                let writer = open_at(&n.parent, &n.name, libc::O_WRONLY, 0)?;
                if ident(&writer)? != ident(&n.file)? {
                    return Err(2);
                }
                self.nodes.get_mut(&id).unwrap().writer = Some(Arc::new(writer));
            }
            Ok(self.nodes[&id].writer.as_ref().unwrap().clone())
        }
        fn stat(&mut self, id: u16) -> Result<[u32; 8]> {
            let m = self.file(id)?.metadata().map_err(error)?;
            let size = if m.is_dir() {
                64
            } else {
                u32::try_from(m.len()).map_err(|_| 27u16)?
            };
            if size > i32::MAX as u32 {
                return Err(27);
            }
            Ok([
                (if m.is_dir() { 0o040000 } else { 0o100000 }) | (m.mode() & 0o777),
                if m.is_dir() {
                    if m.nlink() == 0 {
                        0
                    } else {
                        2
                    }
                } else {
                    m.nlink().min(65535) as u32
                },
                size,
                m.atime().clamp(0, u32::MAX as i64) as u32,
                m.mtime().clamp(0, u32::MAX as i64) as u32,
                m.ctime().clamp(0, u32::MAX as i64) as u32,
                0,
                0,
            ])
        }
        fn listing(&mut self, id: u16) -> Result<Vec<u16>> {
            let file = self.directory(id)?;
            // A fresh directory description avoids sharing readdir's cursor
            // with an earlier snapshot (dup alone would share that cursor).
            let scan = open_at(&file, c".", libc::O_RDONLY | libc::O_DIRECTORY, 0)?;
            use std::os::fd::IntoRawFd;
            let fd = scan.into_raw_fd();
            let dir = unsafe { libc::fdopendir(fd) };
            if dir.is_null() {
                unsafe {
                    libc::close(fd);
                }
                return Err(error(io::Error::last_os_error()));
            }
            struct Close(*mut libc::DIR);
            impl Drop for Close {
                fn drop(&mut self) {
                    unsafe {
                        libc::closedir(self.0);
                    }
                }
            }
            let _close = Close(dir);
            let mut names = Vec::new();
            loop {
                let entry = unsafe { libc::readdir(dir) };
                if entry.is_null() {
                    break;
                }
                let name = unsafe { CStr::from_ptr((*entry).d_name.as_ptr()) }
                    .to_bytes()
                    .to_vec();
                if name == b"." || name == b".." || name.len() > 30 {
                    continue;
                }
                if names.len() == 65533 {
                    return Err(23);
                }
                names.push(name);
            }
            names.sort();
            let mut data = vec![0; 64];
            data[0] = id;
            data[1] = b'.' as u16;
            data[32] = if id == 1 {
                1
            } else {
                let parent = open_at(&file, c"..", libc::O_RDONLY | libc::O_DIRECTORY, 0)?;
                self.id(ident(&parent)?)?
            };
            data[33] = b'.' as u16;
            data[34] = b'.' as u16;
            for name in names {
                let text = component(name.clone())?;
                let mut stat = std::mem::MaybeUninit::<libc::stat>::uninit();
                if unsafe {
                    libc::fstatat(
                        file.as_raw_fd(),
                        text.as_ptr(),
                        stat.as_mut_ptr(),
                        libc::AT_SYMLINK_NOFOLLOW,
                    )
                } < 0
                {
                    continue; // Concurrent host removal is normal.
                }
                let stat = unsafe { stat.assume_init() };
                if stat.st_mode & libc::S_IFMT != libc::S_IFREG
                    && stat.st_mode & libc::S_IFMT != libc::S_IFDIR
                {
                    continue;
                }
                let child = self.id((stat.st_dev as u64, stat.st_ino as u64))?;
                let start = data.len();
                data.resize(start + 32, 0);
                data[start] = child;
                for (cell, byte) in data[start + 1..].iter_mut().zip(name) {
                    *cell = byte as u16;
                }
            }
            Ok(data)
        }
        fn read(&mut self, id: u16, offset: u32, count: usize) -> Result<Vec<u16>> {
            let file = self.file(id)?;
            if self.nodes[&id].directory {
                if offset == 0 || self.nodes[&id].entries.is_none() {
                    let data = self.listing(id)?;
                    self.nodes.get_mut(&id).unwrap().entries = Some(data);
                }
                let entries = self.nodes[&id].entries.as_ref().unwrap();
                let start = (offset as usize).min(entries.len());
                return Ok(entries[start..(start + count).min(entries.len())].to_vec());
            }
            let mut data = vec![0u8; count];
            let n = unsafe {
                libc::pread(
                    file.as_raw_fd(),
                    data.as_mut_ptr().cast(),
                    data.len(),
                    offset as libc::off_t,
                )
            };
            if n < 0 {
                return Err(error(io::Error::last_os_error()));
            }
            Ok(data[..n as usize].iter().map(|&b| b as u16).collect())
        }
        // Keep at most 64 extra capabilities between barriers. Closing a guest
        // descriptor does not force durability, and unlinked dirty files remain
        // flushable until sync. Flush errors retain the failed/pending entries.
        fn prepare_dirty(&mut self, id: u16) -> Result<()> {
            if self.dirty.len() >= 64 && !self.dirty.contains_key(&id) {
                self.sync()?;
            }
            Ok(())
        }
        fn mark_dirty(&mut self, id: u16) -> Result<()> {
            let n = self.nodes.get(&id).ok_or(9u16)?;
            self.dirty
                .insert(id, n.writer.as_ref().unwrap_or(&n.file).clone());
            Ok(())
        }
        fn sync(&mut self) -> Result<()> {
            while let Some((&id, file)) = self.dirty.first_key_value() {
                file.sync_all().map_err(error)?;
                self.dirty.remove(&id);
            }
            Ok(())
        }
    }

    impl Runtime {
        fn hostfs_string(&self, ptr: u32) -> Result<Vec<u8>> {
            let start = linear(ptr as u64);
            let mut bytes = Vec::new();
            for cell in self.memory.cells(start..(start + 4096).min(CELLS)) {
                if cell == 0 {
                    return Ok(bytes);
                }
                if cell > 255 {
                    return Err(22);
                }
                bytes.push(cell as u8);
            }
            Err(37)
        }
        fn hostfs_span(&self, ptr: u32, count: usize) -> Result<usize> {
            let start = linear(ptr as u64);
            if start.checked_add(count).is_none_or(|end| end > CELLS) {
                return Err(14);
            }
            Ok(start)
        }
        fn hostfs_call(&mut self, op: u64, a: &mut [u32; 8]) -> Result<u32> {
            if op == 0 {
                let path = self.hostfs_string(a[0])?;
                return self.hostfs.mount(path).map(u32::from);
            }
            let id = u16::try_from(a[0]).map_err(|_| 9u16)?;
            match op {
                1 => {
                    let name = self.hostfs_string(a[1])?;
                    return self.hostfs.lookup(id, name).map(u32::from);
                }
                2 => {
                    let stat = self.hostfs.stat(id)?;
                    let start = self.hostfs_span(a[1], 16)?;
                    for (i, value) in stat.iter().enumerate() {
                        self.memory.write(start + i * 2, *value as u16);
                        self.memory.write(start + i * 2 + 1, (*value >> 16) as u16);
                    }
                }
                3 => {
                    let count = a[3].min(32767) as usize;
                    let start = self.hostfs_span(a[2], count)?;
                    let data = self.hostfs.read(id, a[1], count)?;
                    self.memory.write_from(start, &data);
                    return Ok(data.len() as u32);
                }
                4 => {
                    let count = a[3].min(32767) as usize;
                    let start = self.hostfs_span(a[2], count)?;
                    let data: Result<Vec<u8>> = self
                        .memory
                        .cells(start..start + count)
                        .map(|c| u8::try_from(c).map_err(|_| 22u16))
                        .collect();
                    let data = data?;
                    self.hostfs.prepare_dirty(id)?;
                    let file = self.hostfs.writable(id)?;
                    let n = if a[4] != 0 {
                        let old = checked(unsafe { libc::fcntl(file.as_raw_fd(), libc::F_GETFL) })?;
                        checked(unsafe {
                            libc::fcntl(file.as_raw_fd(), libc::F_SETFL, old | libc::O_APPEND)
                        })?;
                        let n = unsafe {
                            libc::write(file.as_raw_fd(), data.as_ptr().cast(), data.len())
                        };
                        let saved = io::Error::last_os_error();
                        let end = unsafe { libc::lseek(file.as_raw_fd(), 0, libc::SEEK_CUR) };
                        let restore = unsafe { libc::fcntl(file.as_raw_fd(), libc::F_SETFL, old) };
                        if n < 0 {
                            return Err(error(saved));
                        }
                        checked(restore)?;
                        if end < 0 || end > i32::MAX as libc::off_t {
                            return Err(27);
                        }
                        a[5] = end as u32;
                        n
                    } else {
                        if (a[1] as u64) + data.len() as u64 > i32::MAX as u64 {
                            return Err(27);
                        }
                        let n = unsafe {
                            libc::pwrite(
                                file.as_raw_fd(),
                                data.as_ptr().cast(),
                                data.len(),
                                a[1] as libc::off_t,
                            )
                        };
                        if n < 0 {
                            return Err(error(io::Error::last_os_error()));
                        }
                        a[5] = a[1] + n as u32;
                        n
                    };
                    if n > 0 {
                        self.hostfs.mark_dirty(id)?;
                    }
                    if a[6] != 0 {
                        file.sync_all().map_err(error)?;
                        self.hostfs.dirty.remove(&id);
                    }
                    return Ok(n as u32);
                }
                5 => {
                    self.hostfs.prepare_dirty(id)?;
                    let parent = self.hostfs.directory(id)?;
                    let name = component(self.hostfs_string(a[1])?)?;
                    if a[3] != 0 {
                        checked(unsafe {
                            libc::mkdirat(
                                parent.as_raw_fd(),
                                name.as_ptr(),
                                (a[2] & 0o777) as libc::mode_t,
                            )
                        })?;
                    }
                    let file = open_at(
                        &parent,
                        &name,
                        if a[3] != 0 {
                            libc::O_RDONLY | libc::O_DIRECTORY
                        } else {
                            libc::O_RDWR | libc::O_CREAT | libc::O_EXCL
                        },
                        a[2] & 0o777,
                    )?;
                    let writer = if a[3] == 0 {
                        Some(Arc::new(file.try_clone().map_err(error)?))
                    } else {
                        None
                    };
                    let id = self.hostfs.insert(file, parent, name)?;
                    self.hostfs.nodes.get_mut(&id).unwrap().writer = writer;
                    if a[3] == 0 {
                        self.hostfs.mark_dirty(id)?;
                    }
                    self.hostfs.nodes.get_mut(&(a[0] as u16)).unwrap().entries = None;
                    return Ok(id as u32);
                }
                6 => {
                    let parent = self.hostfs.directory(id)?;
                    let name = component(self.hostfs_string(a[1])?)?;
                    checked(unsafe {
                        libc::unlinkat(
                            parent.as_raw_fd(),
                            name.as_ptr(),
                            if a[2] != 0 { libc::AT_REMOVEDIR } else { 0 },
                        )
                    })?;
                    self.hostfs.nodes.get_mut(&id).unwrap().entries = None;
                }
                7 => {
                    let parent = self.hostfs.directory(a[1] as u16)?;
                    let name = component(self.hostfs_string(a[2])?)?;
                    let node = self.hostfs.nodes.get(&id).ok_or(9u16)?;
                    self.hostfs.beneath(&node.parent)?;
                    let current = open_at(&node.parent, &node.name, libc::O_RDONLY, 0)?;
                    if ident(&current)? != ident(&node.file)? {
                        return Err(2);
                    }
                    checked(unsafe {
                        libc::linkat(
                            node.parent.as_raw_fd(),
                            node.name.as_ptr(),
                            parent.as_raw_fd(),
                            name.as_ptr(),
                            0,
                        )
                    })?;
                    self.hostfs.nodes.get_mut(&(a[1] as u16)).unwrap().entries = None;
                }
                8 => {
                    let from = self.hostfs.directory(id)?;
                    let to = self.hostfs.directory(a[2] as u16)?;
                    let old = component(self.hostfs_string(a[1])?)?;
                    let new = component(self.hostfs_string(a[3])?)?;
                    let source = open_at(&from, &old, libc::O_RDONLY, 0)?;
                    let identity = ident(&source)?;
                    // Refuse replacement of unsupported host objects as well.
                    match open_at(&to, &new, libc::O_RDONLY, 0) {
                        Ok(_) | Err(2) => (),
                        Err(e) => return Err(e),
                    }
                    checked(unsafe {
                        libc::renameat(from.as_raw_fd(), old.as_ptr(), to.as_raw_fd(), new.as_ptr())
                    })?;
                    if let Some(node) = self
                        .hostfs
                        .identities
                        .get(&identity)
                        .and_then(|n| self.hostfs.nodes.get_mut(n))
                    {
                        node.parent = to;
                        node.name = new;
                    }
                    self.hostfs.nodes.get_mut(&id).unwrap().entries = None;
                    self.hostfs.nodes.get_mut(&(a[2] as u16)).unwrap().entries = None;
                }
                9 => {
                    self.hostfs.prepare_dirty(id)?;
                    if a[1] > i32::MAX as u32 {
                        return Err(27);
                    }
                    self.hostfs
                        .writable(id)?
                        .set_len(a[1] as u64)
                        .map_err(error)?;
                    self.hostfs.mark_dirty(id)?;
                }
                10 => {
                    self.hostfs.prepare_dirty(id)?;
                    let file = self.hostfs.file(id)?;
                    checked(unsafe {
                        libc::fchmod(file.as_raw_fd(), (a[1] & 0o777) as libc::mode_t)
                    })?;
                    self.hostfs.mark_dirty(id)?;
                }
                11 => {
                    self.hostfs.prepare_dirty(id)?;
                    let file = self.hostfs.file(id)?;
                    let times = [
                        libc::timespec {
                            tv_sec: a[1] as libc::time_t,
                            tv_nsec: 0,
                        },
                        libc::timespec {
                            tv_sec: a[2] as libc::time_t,
                            tv_nsec: 0,
                        },
                    ];
                    checked(unsafe { libc::futimens(file.as_raw_fd(), times.as_ptr()) })?;
                    self.hostfs.mark_dirty(id)?;
                }
                12 => {
                    if id != 1 {
                        self.hostfs.nodes.remove(&id);
                    }
                }
                13 => self.hostfs.sync()?,
                14 => {
                    if a[1] != 0 {
                        self.hostfs.writable(id)?;
                    }
                }
                15 => return Ok(u32::from(self.hostfs.listing(id)?.len() == 64)),
                _ => return Err(42),
            }
            Ok(0)
        }
    }
    #[no_mangle]
    unsafe extern "C" fn rcc_native_ext_rcc_hosted_fs(
        ctx: *mut Runtime,
        op: u64,
        request: u64,
    ) -> u64 {
        let rt = unsafe { &mut *ctx };
        let start = match rt.hostfs_span(request as u32, 16) {
            Ok(n) => n,
            Err(e) => return (-(e as i32)) as u32 as u64,
        };
        let mut args = [0; 8];
        for (i, arg) in args.iter_mut().enumerate() {
            *arg = rt.memory.read(start + i * 2) as u32
                | ((rt.memory.read(start + i * 2 + 1) as u32) << 16);
        }
        let result = rt.hostfs_call(op, &mut args);
        for (i, arg) in args.iter().enumerate() {
            rt.memory.write(start + i * 2, *arg as u16);
            rt.memory.write(start + i * 2 + 1, (*arg >> 16) as u16);
        }
        match result {
            Ok(n) => n as u64,
            Err(e) => (-(e as i32)) as u32 as u64,
        }
    }

    #[cfg(test)]
    mod tests {
        use super::*;
        use std::{
            path::PathBuf,
            sync::atomic::{AtomicU64, Ordering},
        };
        struct Fixture(PathBuf);
        impl Fixture {
            fn new() -> Self {
                static NEXT: AtomicU64 = AtomicU64::new(0);
                let path = std::env::temp_dir().join(format!(
                    "rcc-hostfs-{}-{}-{}",
                    std::process::id(),
                    std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap()
                        .as_nanos(),
                    NEXT.fetch_add(1, Ordering::Relaxed)
                ));
                std::fs::create_dir(&path).unwrap();
                std::fs::create_dir(path.join("export")).unwrap();
                Self(path)
            }
            fn mount(&self) -> HostFs {
                let mut fs = HostFs::default();
                assert_eq!(
                    fs.mount(self.0.join("export").as_os_str().as_bytes().to_vec()),
                    Ok(1)
                );
                fs
            }
        }
        impl Drop for Fixture {
            fn drop(&mut self) {
                std::fs::remove_dir_all(&self.0).unwrap();
            }
        }
        #[test]
        fn host_replacement_preserves_open_object_and_refreshes_path_lookup() {
            let fixture = Fixture::new();
            let file = fixture.0.join("export/note");
            std::fs::write(&file, b"old").unwrap();
            let mut fs = fixture.mount();
            let old = fs.lookup(1, b"note".to_vec()).unwrap();
            let replacement = fixture.0.join("export/new");
            std::fs::write(&replacement, b"new").unwrap();
            std::fs::rename(replacement, &file).unwrap();
            let new = fs.lookup(1, b"note".to_vec()).unwrap();
            assert_ne!(old, new);
            assert_eq!(fs.read(old, 0, 3).unwrap(), vec![111, 108, 100]);
            assert_eq!(fs.read(new, 0, 3).unwrap(), vec![110, 101, 119]);
        }
        #[test]
        fn symbolic_links_special_files_and_moved_outside_directories_are_rejected() {
            let fixture = Fixture::new();
            std::fs::write(fixture.0.join("outside"), b"secret").unwrap();
            std::os::unix::fs::symlink("../outside", fixture.0.join("export/escape")).unwrap();
            std::fs::create_dir(fixture.0.join("export/moving")).unwrap();
            let mut fs = fixture.mount();
            assert_eq!(fs.lookup(1, b"escape".to_vec()), Err(13));
            let dir = fs.lookup(1, b"moving".to_vec()).unwrap();
            std::fs::rename(fixture.0.join("export/moving"), fixture.0.join("moved")).unwrap();
            assert_eq!(fs.lookup(dir, b"..".to_vec()), Err(13));
            assert_eq!(component(b"../outside".to_vec()), Err(22));
            assert_eq!(component(vec![b'x'; 31]), Err(37));
            let listing = fs.listing(1).unwrap();
            assert_eq!(listing.len(), 64); // . and .. only, no escaping symlink.
        }
        #[test]
        fn dirty_closed_and_unlinked_objects_survive_until_explicit_barrier() {
            let fixture = Fixture::new();
            let path = fixture.0.join("export/dirty");
            std::fs::write(&path, b"old").unwrap();
            let mut fs = fixture.mount();
            let id = fs.lookup(1, b"dirty".to_vec()).unwrap();
            fs.prepare_dirty(id).unwrap();
            let writer = fs.writable(id).unwrap();
            writer.set_len(123).unwrap();
            fs.mark_dirty(id).unwrap();
            std::fs::remove_file(&path).unwrap();
            fs.nodes.remove(&id);
            assert_eq!(fs.dirty.len(), 1);
            assert_eq!(fs.dirty[&id].metadata().unwrap().len(), 123);
            fs.sync().unwrap();
            assert!(fs.dirty.is_empty());
            fs.sync().unwrap();
        }
        #[test]
        fn dirty_capabilities_are_bounded_and_clean_opens_need_no_barrier() {
            let fixture = Fixture::new();
            let mut fs = fixture.mount();
            for n in 0..130 {
                let name = format!("file{n}");
                std::fs::write(fixture.0.join("export").join(&name), b"x").unwrap();
                let id = fs.lookup(1, name.into_bytes()).unwrap();
                fs.writable(id).unwrap();
                fs.prepare_dirty(id).unwrap();
                fs.mark_dirty(id).unwrap();
                fs.nodes.remove(&id);
                assert!(fs.dirty.len() <= 64);
            }
            assert_eq!(fs.dirty.len(), 2);
            fs.sync().unwrap();
            assert!(fs.dirty.is_empty());
            std::fs::write(fixture.0.join("export/clean"), b"").unwrap();
            let id = fs.lookup(1, b"clean".to_vec()).unwrap();
            fs.writable(id).unwrap();
            fs.nodes.remove(&id);
            assert!(fs.dirty.is_empty());
        }
        #[test]
        fn binary_cells_and_shared_identity_remain_distinct_from_host_descriptors() {
            let fixture = Fixture::new();
            let data: Vec<u8> = (0..8193).map(|i| i as u8).collect();
            std::fs::write(fixture.0.join("export/binary"), &data).unwrap();
            std::fs::hard_link(
                fixture.0.join("export/binary"),
                fixture.0.join("export/alias"),
            )
            .unwrap();
            let mut fs = fixture.mount();
            let id = fs.lookup(1, b"binary".to_vec()).unwrap();
            let alias = fs.lookup(1, b"alias".to_vec()).unwrap();
            assert_eq!(id, alias);
            assert_eq!(
                fs.read(id, 0, 8193).unwrap(),
                data.iter().map(|&b| b as u16).collect::<Vec<_>>()
            );
            std::fs::remove_file(fixture.0.join("export/alias")).unwrap();
            fs.lookup(1, b"binary".to_vec()).unwrap();
            assert!(fs.writable(id).is_ok());
            assert_eq!(fs.stat(id).unwrap()[1], 1);
        }
    }
}
#[cfg(unix)]
pub(crate) use unix::HostFs;

#[cfg(not(unix))]
#[derive(Default)]
pub(crate) struct HostFs;
#[cfg(not(unix))]
#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_fs(
    _ctx: *mut crate::Runtime,
    _op: u64,
    _request: u64,
) -> u64 {
    (-42i32) as u32 as u64
}
