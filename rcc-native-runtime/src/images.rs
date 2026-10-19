//! Trusted AOT image loading. Native code is kept alive by its processes;
//! compatibility globals and function tokens are private/relocated separately.
use super::*;
use libloading::Library;
use rcc_ir::native_image::{Manifest, ABI, LEGACY_ABI, MAGIC, MAX_IMAGE_BYTES};
use std::{
    collections::BTreeSet,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, AtomicU32, Ordering},
        mpsc::{self, Receiver, TryRecvError},
        Arc, Mutex, OnceLock, Weak,
    },
};

pub(super) struct Loaded {
    pub template: Vec<u16>,
    pub global_start: usize,
    pub errno: u64,
    tokens: BTreeMap<u32, u32>,
    library: Option<Library>,
    directory: PathBuf,
    collection_needed: Arc<AtomicBool>,
}
impl Drop for Loaded {
    fn drop(&mut self) {
        drop(self.library.take());
        let _ = std::fs::remove_dir_all(&self.directory);
        self.collection_needed.store(true, Ordering::Relaxed);
    }
}
#[derive(Default)]
pub(super) struct Images {
    owners: BTreeMap<u32, Weak<Loaded>>,
    next: Arc<AtomicU32>,
    pub syscall: u32,
    collection_needed: Arc<AtomicBool>,
}
impl Images {
    pub fn owner(&self, token: u32) -> Option<Arc<Loaded>> {
        self.owners.get(&token)?.upgrade()
    }
    pub fn collect(&mut self, functions: &mut BTreeMap<u32, (usize, u32)>) {
        // The scheduler calls this after every resumable step. Scanning all
        // function tokens there made small helper calls cost O(image size).
        // A token can become stale only when its last strong image owner dies.
        if !self.collection_needed.swap(false, Ordering::Relaxed) {
            return;
        }
        self.owners.retain(|token, owner| {
            if owner.strong_count() == 0 {
                functions.remove(token);
                false
            } else {
                true
            }
        });
    }
}

// Workers own only package bytes and immutable native code. They never access
// Runtime, guest memory, process continuations, or the kernel scheduler.
struct Prepared {
    entry: u32,
    owner: Arc<Loaded>,
    functions: std::vec::IntoIter<(u32, (usize, u32))>,
}
pub(super) struct Preparation {
    result: Receiver<Result<Prepared, String>>,
    ready: Option<Prepared>,
    cancelled: Arc<AtomicBool>,
}
impl Drop for Preparation {
    fn drop(&mut self) {
        self.cancelled.store(true, Ordering::Relaxed);
    }
}
struct Job {
    bytes: Vec<u8>,
    next: Arc<AtomicU32>,
    collection_needed: Arc<AtomicBool>,
    cancelled: Arc<AtomicBool>,
    result: mpsc::Sender<Result<Prepared, String>>,
}
fn workers() -> Result<&'static mpsc::SyncSender<Job>, String> {
    static WORKERS: OnceLock<Result<mpsc::SyncSender<Job>, String>> = OnceLock::new();
    WORKERS
        .get_or_init(|| {
            // At most two native loader calls run concurrently; queued work is
            // bounded by the hosted process cap. Dropping a process cancels its job.
            let (send, receive) = mpsc::sync_channel::<Job>(64);
            let receive = Arc::new(Mutex::new(receive));
            for index in 0..2 {
                let receive = receive.clone();
                std::thread::Builder::new()
                    .name(format!("fuse-x-loader-{index}"))
                    .spawn(move || loop {
                        let Ok(job) = receive.lock().unwrap().recv() else {
                            break;
                        };
                        if !job.cancelled.load(Ordering::Relaxed) {
                            let prepared =
                                prepare_image(&job.bytes, &job.next, job.collection_needed);
                            // A disconnected receiver drops the library and its files.
                            let _ = job.result.send(prepared);
                        }
                    })
                    .map_err(|e| e.to_string())?;
            }
            Ok(send)
        })
        .as_ref()
        .map_err(Clone::clone)
}
fn prepare_image(
    bytes: &[u8],
    allocator: &AtomicU32,
    collection_needed: Arc<AtomicBool>,
) -> Result<Prepared, String> {
    if bytes.len() < 16 || bytes.len() > MAX_IMAGE_BYTES || &bytes[..8] != MAGIC {
        return Err("invalid executable header".into());
    }
    let json_size = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
    let code_size = u32::from_le_bytes(bytes[12..16].try_into().unwrap()) as usize;
    let end = 16usize
        .checked_add(json_size)
        .filter(|n| *n <= bytes.len())
        .ok_or("invalid manifest length")?;
    if code_size == 0 || code_size != bytes.len() - end {
        return Err("invalid native code length".into());
    }
    let manifest: Manifest =
        serde_json::from_slice(&bytes[16..end]).map_err(|e| format!("invalid manifest: {e}"))?;
    if !matches!(manifest.abi, ABI | LEGACY_ABI)
        || manifest.arch != std::env::consts::ARCH
        || manifest.os != std::env::consts::OS
    {
        return Err("incompatible native executable ABI, CPU or host OS".into());
    }
    manifest.global_capacity()?;
    if manifest.functions.is_empty() || manifest.functions.len() > 65535 {
        return Err("invalid application memory/function requirements".into());
    }
    let mut local = BTreeSet::new();
    let mut symbols = BTreeSet::new();
    for f in &manifest.functions {
        if f.token < 0x8000_0000
            || !local.insert(f.token)
            || !symbols.insert(&f.symbol)
            || !f.symbol.starts_with("rcc_user_")
            || f.symbol.as_bytes().contains(&0)
            || f.arity > 65535
        {
            return Err("invalid function table".into());
        }
    }
    if !manifest
        .functions
        .iter()
        .any(|f| f.token == manifest.entry && f.arity == 1)
    {
        return Err("missing application entry".into());
    }
    if manifest.errno != 0 && manifest.global_index(manifest.errno, 1).is_err() {
        return Err("invalid errno address".into());
    }
    let mut helpers = Vec::new();
    let mut names = BTreeSet::new();
    for helper in &manifest.helpers {
        if !names.insert(&helper.symbol) {
            return Err("duplicate helper binding".into());
        }
        helpers.push(
            image_bindings::resolve(&helper.symbol, helper.arity)
                .ok_or_else(|| format!("unsupported runtime helper {}", helper.symbol))?
                as u64,
        );
    }
    // Check all data writes before executing any code or changing process memory.
    for &(p, v, n, relocate) in &manifest.initializers {
        if !(1..=4).contains(&n)
            || manifest.global_index(p, n).is_err()
            || (relocate && (n != 2 || !local.contains(&(v as u32)) || v > u64::from(u32::MAX)))
        {
            return Err("invalid global initializer/relocation".into());
        }
    }
    let first = allocator
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |old| {
            old.max(0x9000_0000)
                .checked_add(manifest.functions.len() as u32)
                .filter(|n| *n < u32::MAX)
        })
        .map_err(|_| "native function token space exhausted")?
        .max(0x9000_0000);
    let tokens: BTreeMap<_, _> = manifest
        .functions
        .iter()
        .enumerate()
        .map(|(i, f)| (f.token, first + i as u32))
        .collect();
    let mut template = vec![0; manifest.global_cells as usize];
    for &(p, mut v, n, relocate) in &manifest.initializers {
        if relocate {
            v = u64::from(tokens[&(v as u32)]);
        }
        for i in 0..n as usize {
            template[manifest.global_index(p, n)? + i] = (v >> (16 * i)) as u16;
        }
    }
    let directory = std::env::temp_dir().join(format!(
        "fuse-x-image-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_err(|e| e.to_string())?
            .as_nanos()
    ));
    let mut directories = std::fs::DirBuilder::new();
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;
        directories.mode(0o700);
    }
    directories.create(&directory).map_err(|e| e.to_string())?;
    let path = directory.join("application.dylib");
    let loaded = (|| -> Result<_, String> {
        use std::io::Write;
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&path)
            .map_err(|e| e.to_string())?;
        file.write_all(&bytes[end..]).map_err(|e| e.to_string())?;
        drop(file);
        // Packages contain trusted native code, not sandboxed bytecode.
        let library = unsafe { Library::new(&path) }.map_err(|e| format!("native loader: {e}"))?;
        let mut functions = Vec::new();
        for f in &manifest.functions {
            let address = unsafe {
                library.get::<unsafe extern "C" fn(*mut Runtime) -> u64>(f.symbol.as_bytes())
            }
            .map_err(|e| e.to_string())?;
            functions.push((tokens[&f.token], (*address as *const () as usize, f.arity)));
        }
        let bind = unsafe { library.get::<unsafe extern "C" fn(*const u64)>(b"fuse_x_bind") }
            .map_err(|e| e.to_string())?;
        unsafe { bind(helpers.as_ptr()) };
        Ok((library, functions))
    })();
    let (library, functions) = match loaded {
        Ok(value) => value,
        Err(e) => {
            let _ = std::fs::remove_dir_all(&directory);
            return Err(e);
        }
    };
    let entry = tokens[&manifest.entry];
    let owner = Arc::new(Loaded {
        template,
        global_start: manifest.global_bank as usize * BANK,
        errno: manifest.errno,
        tokens,
        library: Some(library),
        directory,
        collection_needed,
    });
    Ok(Prepared {
        entry,
        owner,
        functions: functions.into_iter(),
    })
}
impl Runtime {
    fn commit_image(&mut self, prepared: &mut Prepared, limit: usize) -> bool {
        for (token, entry) in prepared.functions.by_ref().take(limit) {
            self.functions.insert(token, entry);
            self.images
                .owners
                .insert(token, Arc::downgrade(&prepared.owner));
        }
        prepared.functions.len() == 0
    }
    fn load_image(&mut self, bytes: &[u8]) -> Result<u32, String> {
        let mut prepared = prepare_image(
            bytes,
            &self.images.next,
            self.images.collection_needed.clone(),
        )?;
        self.commit_image(&mut prepared, usize::MAX);
        let entry = prepared.entry;
        self.processes.stage_image(entry, prepared.owner);
        Ok(entry)
    }
    fn poll_image(&mut self) -> Result<Option<u32>, String> {
        let mut job = self
            .processes
            .preparing()
            .take()
            .ok_or("no pending executable")?;
        if job.ready.is_none() {
            match job.result.try_recv() {
                Ok(result) => job.ready = Some(result?),
                Err(TryRecvError::Empty) => {}
                Err(TryRecvError::Disconnected) => {
                    return Err("executable worker disconnected".into())
                }
            }
        }
        if let Some(prepared) = &mut job.ready {
            if self.commit_image(prepared, 128) {
                let prepared = job.ready.take().unwrap();
                let entry = prepared.entry;
                self.processes.stage_image(entry, prepared.owner);
                return Ok(Some(entry));
            }
        }
        *self.processes.preparing() = Some(job);
        Ok(None)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn image(images: &Images, name: &str) -> Arc<Loaded> {
        let directory = std::env::temp_dir().join(format!(
            "rcc-image-collection-{}-{}-{}",
            std::process::id(),
            name,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&directory).unwrap();
        Arc::new(Loaded {
            template: Vec::new(),
            global_start: BANK,
            errno: 0,
            tokens: BTreeMap::new(),
            library: None,
            directory,
            collection_needed: images.collection_needed.clone(),
        })
    }

    fn pending(rt: &mut Runtime) -> (mpsc::Sender<Result<Prepared, String>>, Arc<AtomicBool>) {
        let (send, result) = mpsc::channel();
        let cancelled = Arc::new(AtomicBool::new(false));
        *rt.processes.preparing() = Some(Preparation {
            result,
            ready: None,
            cancelled: cancelled.clone(),
        });
        (send, cancelled)
    }
    #[test]
    fn preparation_is_nonblocking_and_cancellation_drops_late_results() {
        let mut rt = Runtime::new();
        rt.processes = process::Machine::test_hosted();
        let (send, cancelled) = pending(&mut rt);
        assert_eq!(rt.poll_image().unwrap(), None); // Worker has not finished.
        let owner = image(&rt.images, "cancelled");
        let directory = owner.directory.clone();
        *rt.processes.preparing() = None;
        assert!(cancelled.load(Ordering::Relaxed));
        let result = send.send(Ok(Prepared {
            entry: 0x90000000,
            owner,
            functions: vec![].into_iter(),
        }));
        assert!(result.is_err());
        drop(result);
        assert!(!directory.exists());
        assert!(rt.functions.is_empty());
    }
    #[test]
    fn image_is_staged_only_after_bounded_commit_and_partial_commit_can_be_cancelled() {
        let mut rt = Runtime::new();
        rt.processes = process::Machine::test_hosted();
        rt.functions.insert(1, (123, 0));
        for cancel in [true, false] {
            let (send, _) = pending(&mut rt);
            let owner = image(&rt.images, if cancel { "partial" } else { "complete" });
            let directory = owner.directory.clone();
            let entry = 0x90000000;
            send.send(Ok(Prepared {
                entry,
                owner,
                functions: (entry..entry + 300)
                    .map(|token| (token, (456, 1)))
                    .collect::<Vec<_>>()
                    .into_iter(),
            }))
            .ok()
            .unwrap();
            assert_eq!(rt.poll_image().unwrap(), None);
            assert_eq!(rt.functions.len(), 129);
            rt.processes.discard_staged(entry); // Not staged yet.
            assert!(directory.exists());
            if cancel {
                *rt.processes.preparing() = None;
            } else {
                assert_eq!(rt.poll_image().unwrap(), None);
                assert_eq!(rt.functions.len(), 257);
                assert_eq!(rt.poll_image().unwrap(), Some(entry));
                assert_eq!(rt.functions.len(), 301);
                assert!(rt.processes.preparing().is_none());
                assert!(directory.exists());
                rt.processes.discard_staged(entry);
            }
            assert!(!directory.exists());
            rt.images.collect(&mut rt.functions);
            assert_eq!(rt.functions, BTreeMap::from([(1, (123, 0))]));
        }
    }
    #[test]
    fn rejected_preparation_does_not_replace_the_running_program() {
        let mut rt = Runtime::new();
        rt.processes = process::Machine::test_hosted();
        rt.functions.insert(1, (123, 0));
        let (send, _) = pending(&mut rt);
        send.send(Err("bad manifest".into())).ok().unwrap();
        assert_eq!(rt.poll_image(), Err("bad manifest".into()));
        assert!(rt.processes.preparing().is_none());
        assert_eq!(rt.functions, BTreeMap::from([(1, (123, 0))]));
    }
    #[test]
    fn worker_pool_rejects_malformed_packages_without_guest_memory() {
        let (send, receive) = mpsc::channel();
        workers()
            .unwrap()
            .try_send(Job {
                bytes: b"not an executable".to_vec(),
                result: send,
                next: Arc::default(),
                collection_needed: Arc::default(),
                cancelled: Arc::default(),
            })
            .ok()
            .unwrap();
        assert_eq!(
            receive
                .recv_timeout(std::time::Duration::from_secs(5))
                .unwrap()
                .err()
                .unwrap(),
            "invalid executable header"
        );
    }

    #[test]
    fn collection_waits_for_last_image_owner_and_keeps_other_images() {
        let mut images = Images::default();
        let mut functions = BTreeMap::from([(1, (123, 0))]); // Prelinked kernel.
        let first = image(&images, "first");
        let second = image(&images, "second");
        for token in 0x9000_0000..0x9000_1000 {
            images.owners.insert(token, Arc::downgrade(&first));
            functions.insert(token, (456, 0));
        }
        let second_token = 0x9000_1000;
        images.owners.insert(second_token, Arc::downgrade(&second));
        functions.insert(second_token, (789, 0));
        let retained = first.clone(); // Another process/frame still uses it.
        drop(first);
        assert!(!images.collection_needed.load(Ordering::Relaxed));
        images.collect(&mut functions);
        assert_eq!(functions.len(), 4098);

        let directory = retained.directory.clone();
        drop(retained);
        assert!(!directory.exists());
        assert!(images.collection_needed.load(Ordering::Relaxed));
        images.collect(&mut functions);
        assert!(!images.collection_needed.load(Ordering::Relaxed));
        assert_eq!(images.owners.len(), 1);
        assert_eq!(
            functions,
            BTreeMap::from([(1, (123, 0)), (second_token, (789, 0))])
        );
        assert!(Arc::ptr_eq(&images.owner(second_token).unwrap(), &second));

        drop(second);
        images.collect(&mut functions);
        assert!(images.owners.is_empty());
        assert_eq!(functions, BTreeMap::from([(1, (123, 0))]));
        images.collect(&mut functions);
        assert_eq!(functions.len(), 1);
    }
}

#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_valid_function(
    ctx: *mut Runtime,
    token: u64,
    arity: u64,
) -> u64 {
    let rt = unsafe { &mut *ctx };
    // Dynamic tokens must belong to the currently executing program. A token
    // retained from an earlier exec must never enter an unrelated image.
    let current = rt.images.owner(rt.processes.current_token());
    let owner = rt.images.owner(token as u32);
    let same = match (current, owner) {
        (Some(a), Some(b)) => Arc::ptr_eq(&a, &b),
        // The caller is kernel code; consult the active application's owner.
        (None, Some(b)) => rt.processes.owns_image(&b),
        (None, None) => rt.processes.is_prelinked(),
        _ => false,
    };
    u64::from(
        token <= u64::from(u32::MAX)
            && same
            && rt
                .functions
                .get(&(token as u32))
                .is_some_and(|f| u64::from(f.1) == arity),
    )
}

#[no_mangle]
pub(super) unsafe extern "C" fn rcc_native_image_token(ctx: *mut Runtime, local: u64) -> u64 {
    let rt = unsafe { &mut *ctx };
    let token = rt.processes.current_token();
    let owner = rt
        .images
        .owner(token)
        .unwrap_or_else(|| fatal("application token outside its image"));
    u64::from(
        *owner
            .tokens
            .get(&(local as u32))
            .unwrap_or_else(|| fatal("unknown application function token")),
    )
}
#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_image_load(
    ctx: *mut Runtime,
    pointer: u64,
    count: u64,
) -> u64 {
    let rt = unsafe { &mut *ctx };
    if !rt.processes.hosted || count > MAX_IMAGE_BYTES as u64 {
        return 0;
    }
    let start = memory::linear(pointer);
    if start < address_space::KERNEL_START || count as usize > CELLS - start {
        return 0;
    }
    let mut cells = vec![0; count as usize];
    rt.memory.read_into(start, &mut cells);
    if cells.iter().any(|c| *c > 255) {
        return 0;
    }
    let bytes: Vec<_> = cells.into_iter().map(|c| c as u8).collect();
    match rt.load_image(&bytes) {
        Ok(entry) => u64::from(entry),
        Err(e) => {
            eprintln!("Fuse-X: executable rejected: {e}");
            0
        }
    }
}
#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_image_discard(ctx: *mut Runtime, entry: u64) -> u64 {
    let rt = unsafe { &mut *ctx };
    rt.processes.discard_staged(entry as u32);
    0
}
#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_bind_syscall(ctx: *mut Runtime, entry: u64) -> u64 {
    let rt = unsafe { &mut *ctx };
    if !rt.processes.hosted
        || rt
            .functions
            .get(&(entry as u32))
            .is_none_or(|(_, arity)| *arity != 1)
    {
        fatal("invalid hosted syscall entry");
    }
    rt.images.syscall = entry as u32;
    0
}

#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_image_begin(ctx: *mut Runtime, cells: u64) -> u64 {
    let rt = unsafe { &mut *ctx };
    *rt.processes.loading() = None;
    *rt.processes.preparing() = None;
    if !rt.processes.hosted || !(16..=MAX_IMAGE_BYTES as u64).contains(&cells) {
        return 0;
    }
    let mut bytes = Vec::new();
    if bytes.try_reserve_exact(cells as usize).is_err() {
        return 0;
    }
    *rt.processes.loading() = Some((cells as usize, bytes));
    1
}

#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_image_append(
    ctx: *mut Runtime,
    pointer: u64,
    count: u64,
) -> u64 {
    let rt = unsafe { &mut *ctx };
    if !rt.processes.hosted || pointer > u64::from(u32::MAX) || pointer & 65535 >= BANK as u64 {
        return 0;
    }
    let start = memory::linear(pointer);
    if start < address_space::KERNEL_START || count > (CELLS - start) as u64 {
        return 0;
    }
    let Some((expected, bytes)) = rt.processes.loading().as_ref() else {
        return 0;
    };
    if count > (expected - bytes.len()) as u64 {
        return 0;
    }
    let mut cells = vec![0; count as usize];
    rt.memory.read_into(start, &mut cells);
    if cells.iter().any(|c| *c > 255) {
        return 0;
    }
    rt.processes
        .loading()
        .as_mut()
        .unwrap()
        .1
        .extend(cells.into_iter().map(|c| c as u8));
    1
}

#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_image_finish(ctx: *mut Runtime) -> u64 {
    let rt = unsafe { &mut *ctx };
    let Some((expected, bytes)) = rt.processes.loading().take() else {
        return 0;
    };
    if bytes.len() != expected {
        return 0;
    }
    match rt.load_image(&bytes) {
        Ok(entry) => u64::from(entry),
        Err(e) => {
            eprintln!("Fuse-X: executable rejected: {e}");
            0
        }
    }
}

#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_image_cancel(ctx: *mut Runtime) -> u64 {
    let rt = unsafe { &mut *ctx };
    *rt.processes.loading() = None;
    *rt.processes.preparing() = None;
    0
}

#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_image_prepare(ctx: *mut Runtime) -> u64 {
    let rt = unsafe { &mut *ctx };
    let Some((expected, bytes)) = rt.processes.loading().take() else {
        return 0;
    };
    if bytes.len() != expected || rt.processes.preparing().is_some() {
        return 0;
    }
    let (send, receive) = mpsc::channel();
    let cancelled = Arc::new(AtomicBool::new(false));
    let job = Job {
        bytes,
        result: send,
        cancelled: cancelled.clone(),
        next: rt.images.next.clone(),
        collection_needed: rt.images.collection_needed.clone(),
    };
    let Ok(pool) = workers() else { return 0 };
    if pool.try_send(job).is_err() {
        return 0;
    }
    *rt.processes.preparing() = Some(Preparation {
        result: receive,
        ready: None,
        cancelled,
    });
    1
}
#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_image_poll(ctx: *mut Runtime) -> u64 {
    let rt = unsafe { &mut *ctx };
    match rt.poll_image() {
        Ok(entry) => u64::from(entry.unwrap_or(0)),
        Err(e) => {
            eprintln!("Fuse-X: executable rejected: {e}");
            u64::from(u32::MAX)
        }
    }
}
