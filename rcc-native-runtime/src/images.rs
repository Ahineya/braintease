//! Trusted AOT image loading. Native code is kept alive by its processes;
//! compatibility globals and function tokens are private/relocated separately.
use super::*;
use libloading::Library;
use rcc_ir::native_image::{Manifest, ABI, MAGIC, MAX_IMAGE_BYTES};
use std::{
    collections::BTreeSet,
    path::PathBuf,
    sync::{Arc, Weak},
};

pub(super) struct Loaded {
    pub template: Vec<u16>,
    pub errno: u64,
    tokens: BTreeMap<u32, u32>,
    library: Option<Library>,
    directory: PathBuf,
}
impl Drop for Loaded {
    fn drop(&mut self) {
        drop(self.library.take());
        let _ = std::fs::remove_dir_all(&self.directory);
    }
}
#[derive(Default)]
pub(super) struct Images {
    owners: BTreeMap<u32, Weak<Loaded>>,
    next: u32,
    pub syscall: u32,
}
impl Images {
    pub fn owner(&self, token: u32) -> Option<Arc<Loaded>> {
        self.owners.get(&token)?.upgrade()
    }
    pub fn collect(&mut self, functions: &mut BTreeMap<u32, (usize, u32)>) {
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

impl Runtime {
    fn load_image(&mut self, bytes: &[u8]) -> Result<u32, String> {
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
        let manifest: Manifest = serde_json::from_slice(&bytes[16..end])
            .map_err(|e| format!("invalid manifest: {e}"))?;
        if manifest.abi != ABI
            || manifest.arch != std::env::consts::ARCH
            || manifest.os != std::env::consts::OS
        {
            return Err("incompatible native executable ABI, CPU or host OS".into());
        }
        if manifest.global_cells == 0
            || manifest.global_cells as usize >= BANK
            || manifest.functions.is_empty()
            || manifest.functions.len() > 65535
        {
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
        if manifest.errno != 0
            && !(0x10001..0x10000 + u64::from(manifest.global_cells)).contains(&manifest.errno)
        {
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
                || p >> 16 != 1
                || p & 65535 == 0
                || (p & 65535) + n > u64::from(manifest.global_cells)
                || (relocate && (n != 2 || !local.contains(&(v as u32)) || v > u64::from(u32::MAX)))
            {
                return Err("invalid global initializer/relocation".into());
            }
        }
        let first = self.images.next.max(0x9000_0000);
        let next = first
            .checked_add(manifest.functions.len() as u32)
            .filter(|n| *n < 0xffff_ffff)
            .ok_or("native function token space exhausted")?;
        let tokens: BTreeMap<_, _> = manifest
            .functions
            .iter()
            .enumerate()
            .map(|(i, f)| (f.token, first + i as u32))
            .collect();
        let mut template = vec![0; BANK];
        for &(p, mut v, n, relocate) in &manifest.initializers {
            if relocate {
                v = u64::from(tokens[&(v as u32)]);
            }
            for i in 0..n as usize {
                template[(p & 65535) as usize + i] = (v >> (16 * i)) as u16;
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
            let library =
                unsafe { Library::new(&path) }.map_err(|e| format!("native loader: {e}"))?;
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
            errno: manifest.errno,
            tokens,
            library: Some(library),
            directory,
        });
        for (token, entry) in functions {
            self.functions.insert(token, entry);
            self.images.owners.insert(token, Arc::downgrade(&owner));
        }
        self.images.next = next;
        self.processes.stage_image(entry, owner);
        Ok(entry)
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
    let cells: Vec<_> = rt.memory.cells(start..start + count as usize).collect();
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
    let cells: Vec<_> = rt.memory.cells(start..start + count as usize).collect();
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
    0
}
