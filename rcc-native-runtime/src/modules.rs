//! Immutable host code is cached by package contents. Every executable load
//! owns a separate binding graph and initialization template; no process data
//! or imported guest address is patched into the shared native library.
use super::*;
use rcc_ir::native_image::{self as format, ModuleMetadata};

pub(super) struct Code {
    manifest: Manifest,
    data: Vec<u16>,
    functions: Vec<(u32, usize, u32)>,
    library: Option<Library>,
    directory: PathBuf,
}
impl Drop for Code {
    fn drop(&mut self) {
        drop(self.library.take());
        let _ = std::fs::remove_dir_all(&self.directory);
    }
}
pub(super) struct Instance {
    code: Arc<Code>,
    base: usize,
    tokens: BTreeMap<u32, u32>,
    data_imports: BTreeMap<u64, u64>,
}
fn pointer(linear: usize) -> u64 {
    ((linear / BANK) as u64) << 16 | (linear % BANK) as u64
}
impl Instance {
    pub(super) fn token(&self, local: u32) -> Option<u32> {
        self.tokens.get(&local).copied()
    }
    pub(super) fn global(&self, local: u64) -> Result<u64, String> {
        if let Some(pointer) = self.data_imports.get(&local) {
            return Ok(*pointer);
        }
        let index = self.code.manifest.global_index(local, 1)?;
        Ok(pointer(self.base + index))
    }
}
impl Loaded {
    pub(super) fn instance(&self, token: u32) -> Option<&Instance> {
        self.function_modules
            .get(&token)
            .and_then(|i| self.modules.get(*i))
    }
}

pub(super) fn validate(m: &Manifest) -> Result<&ModuleMetadata, String> {
    if m.abi != MODULE_ABI
        || m.arch != std::env::consts::ARCH
        || m.os != std::env::consts::OS
        || m.functions.is_empty()
        || m.functions.len() > 65535
        || !m.initializers.is_empty()
    {
        return Err("incompatible shared module".into());
    }
    let module = m.module.as_ref().ok_or("missing module metadata")?;
    if !module.name.is_empty() && !valid_name(&module.name) {
        return Err("invalid library name".into());
    }
    let mut tokens = BTreeSet::new();
    let mut symbols = BTreeSet::new();
    for f in &m.functions {
        if !(0x8000_0000..0xC000_0000).contains(&f.token)
            || !tokens.insert(f.token)
            || !symbols.insert(&f.symbol)
            || !f.symbol.starts_with("rcc_user_")
            || f.symbol.as_bytes().contains(&0)
            || f.arity > 65535
        {
            return Err("invalid module function table".into());
        }
    }
    let mut deps = BTreeSet::new();
    for dep in &module.dependencies {
        if !valid_name(&dep.name)
            || !deps.insert(&dep.name)
            || !dep.path.starts_with('/')
            || dep.path.len() > 500
            || dep.path.as_bytes().contains(&0)
            || dep.path.split('/').any(|c| c == "..")
        {
            return Err("invalid module dependency".into());
        }
    }
    symbols.clear();
    for s in &module.exports {
        if !symbols.insert(&s.name) || s.name.is_empty() || s.signature.len() > 65535 {
            return Err("invalid module export".into());
        }
        if s.function {
            if s.value > u64::from(u32::MAX) || !tokens.contains(&(s.value as u32)) {
                return Err("invalid function export".into());
            }
        } else {
            m.global_index(s.value, 1)?;
        }
    }
    let mut imports = BTreeSet::new();
    for i in &module.imports {
        if !(0xC000_0000..=u64::from(u32::MAX)).contains(&i.local)
            || !imports.insert(i.local)
            || !deps.contains(&i.library)
        {
            return Err("invalid module import".into());
        }
    }
    for r in &module.relocations {
        if r.offset == 0
            || u64::from(r.offset) + 2 > u64::from(m.global_cells)
            || (r.function && r.addend != 0)
        {
            return Err("invalid module relocation".into());
        }
    }
    Ok(module)
}
pub(super) fn valid_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 128
        && name
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))
}

fn cache() -> &'static Mutex<BTreeMap<String, Weak<Code>>> {
    static CACHE: OnceLock<Mutex<BTreeMap<String, Weak<Code>>>> = OnceLock::new();
    CACHE.get_or_init(Default::default)
}
pub(super) fn cached(name: &str, interface: &str) -> Option<Arc<Code>> {
    let cache = cache().lock().ok()?;
    cache.values().filter_map(Weak::upgrade).find(|code| {
        let module = code.manifest.module.as_ref().unwrap();
        module.name == name && format::interface(module) == interface
    })
}
impl Code {
    pub(super) fn manifest(&self) -> &Manifest {
        &self.manifest
    }
}
fn code(bytes: &[u8]) -> Result<Arc<Code>, String> {
    let mut cache = cache().lock().map_err(|_| "module cache poisoned")?;
    cache.retain(|_, code| code.strong_count() != 0);
    let id = format::content_id(bytes);
    if let Some(code) = cache.get(&id).and_then(Weak::upgrade) {
        return Ok(code);
    }
    let (manifest, data, payload) = format::decode(bytes)?;
    validate(&manifest)?;
    let mut helpers = Vec::new();
    let mut names = BTreeSet::new();
    for h in &manifest.helpers {
        if !names.insert(&h.symbol) {
            return Err("duplicate module helper".into());
        }
        helpers.push(
            image_bindings::resolve(&h.symbol, h.arity)
                .ok_or_else(|| format!("unsupported runtime helper {}", h.symbol))?
                as u64,
        );
    }
    let directory = std::env::temp_dir().join(format!("fuse-x-module-{}-{id}", std::process::id()));
    let mut builder = std::fs::DirBuilder::new();
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;
        builder.mode(0o700);
    }
    builder.create(&directory).map_err(|e| e.to_string())?;
    let result = (|| -> Result<_, String> {
        use std::io::Write;
        let path = directory.join("module.dylib");
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&path)
            .map_err(|e| e.to_string())?;
        file.write_all(payload).map_err(|e| e.to_string())?;
        drop(file);
        let library = unsafe { Library::new(&path) }.map_err(|e| e.to_string())?;
        let mut functions = Vec::new();
        for f in &manifest.functions {
            let address = unsafe {
                library.get::<unsafe extern "C" fn(*mut Runtime) -> u64>(f.symbol.as_bytes())
            }
            .map_err(|e| e.to_string())?;
            functions.push((f.token, *address as *const () as usize, f.arity));
        }
        let bind = unsafe { library.get::<unsafe extern "C" fn(*const u64)>(b"fuse_x_bind") }
            .map_err(|e| e.to_string())?;
        unsafe { bind(helpers.as_ptr()) };
        Ok((library, functions))
    })();
    let (library, functions) = match result {
        Ok(v) => v,
        Err(e) => {
            let _ = std::fs::remove_dir_all(&directory);
            return Err(e);
        }
    };
    let mut template = vec![0; manifest.global_cells as usize];
    for (i, w) in data.chunks_exact(2).enumerate() {
        template[i] = u16::from_le_bytes([w[0], w[1]]);
    }
    if std::env::var_os("FUSE_X_MODULE_TRACE").is_some() {
        eprintln!(
            "Fuse-X: loaded shared code {} {} ({} functions)",
            manifest.module.as_ref().unwrap().name,
            &id[..12],
            functions.len()
        );
    }
    let code = Arc::new(Code {
        manifest,
        data: template,
        functions,
        library: Some(library),
        directory,
    });
    cache.insert(id, Arc::downgrade(&code));
    Ok(code)
}

pub(super) fn prepare(
    bytes: &[u8],
    libraries: &[Vec<u8>],
    allocator: &AtomicU32,
    collection_needed: Arc<AtomicBool>,
    cached: &[Arc<Code>],
) -> Result<Prepared, String> {
    if libraries.len() + cached.len() > 31 {
        return Err("too many shared modules".into());
    }
    let mut codes = vec![code(bytes)?];
    for bytes in libraries {
        codes.push(code(bytes)?);
    }
    codes.extend_from_slice(cached);
    let mut named = BTreeMap::new();
    for (index, c) in codes.iter().enumerate() {
        let m = c.manifest.module.as_ref().unwrap();
        if (index == 0) != m.name.is_empty()
            || (index > 0 && named.insert(m.name.clone(), index).is_some())
        {
            return Err("invalid/duplicate module name".into());
        }
    }
    for c in &codes {
        for d in &c.manifest.module.as_ref().unwrap().dependencies {
            let target = *named
                .get(&d.name)
                .ok_or_else(|| format!("missing library {}", d.name))?;
            if format::interface(codes[target].manifest.module.as_ref().unwrap()) != d.interface {
                return Err(format!("library ABI mismatch: {}", d.name));
            }
        }
    }
    let mut cursor = format::GLOBAL_BANK as usize * BANK;
    let global_start = cursor;
    let mut template = Vec::new();
    let mut instances = Vec::new();
    let mut functions = Vec::new();
    let mut function_modules = BTreeMap::new();
    for (index, code) in codes.iter().enumerate() {
        let count = code.functions.len() as u32;
        let first = allocator
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |old| {
                old.max(0x9000_0000)
                    .checked_add(count)
                    .filter(|n| *n < 0xFFFF_FFFE)
            })
            .map_err(|_| "native function token space exhausted")?
            .max(0x9000_0000);
        let mut tokens = BTreeMap::new();
        for (i, (local, address, arity)) in code.functions.iter().enumerate() {
            let token = first + i as u32;
            tokens.insert(*local, token);
            functions.push((token, (*address, *arity)));
            function_modules.insert(token, index);
        }
        instances.push(Instance {
            code: code.clone(),
            base: cursor,
            tokens,
            data_imports: BTreeMap::new(),
        });
        cursor = cursor
            .checked_add(code.data.len())
            .ok_or("module data overflow")?;
        if cursor - global_start > format::GLOBAL_BANKS as usize * BANK {
            return Err("combined module globals exceed process capacity".into());
        }
        template.extend_from_slice(&code.data);
    }
    for index in 0..instances.len() {
        for i in &codes[index].manifest.module.as_ref().unwrap().imports {
            let target = &instances[named[&i.library]];
            let export = target
                .code
                .manifest
                .module
                .as_ref()
                .unwrap()
                .exports
                .iter()
                .find(|s| s.name == i.symbol.name)
                .ok_or_else(|| format!("missing symbol {} in {}", i.symbol.name, i.library))?;
            if export.function != i.symbol.function || export.signature != i.symbol.signature {
                return Err(format!("symbol ABI mismatch: {}", export.name));
            }
            if export.function {
                let token = target.tokens[&(export.value as u32)];
                instances[index].tokens.insert(i.local as u32, token);
            } else {
                let value = target.global(export.value)?;
                instances[index].data_imports.insert(i.local, value);
            }
        }
    }
    for (index, instance) in instances.iter().enumerate() {
        for r in &codes[index].manifest.module.as_ref().unwrap().relocations {
            let value = if r.function {
                u64::from(
                    instance
                        .token(u32::try_from(r.target).map_err(|_| "invalid function relocation")?)
                        .ok_or("unknown function relocation")?,
                )
            } else {
                instance.global(r.target)?
            };
            let value = if r.addend != 0 {
                let base = (value >> 16) as i64 * BANK as i64 + (value & 65535) as i64;
                let linear = base
                    .checked_add(r.addend)
                    .filter(|p| *p >= 0 && *p < address_space::KERNEL_START as i64)
                    .ok_or("module pointer relocation overflow")?;
                pointer(linear as usize)
            } else {
                value
            };
            let at = instance.base - global_start + r.offset as usize;
            template[at] = value as u16;
            template[at + 1] = (value >> 16) as u16;
        }
    }
    let main = codes[0].manifest.entry;
    if !codes[0]
        .manifest
        .functions
        .iter()
        .any(|f| f.token == main && f.arity == 1)
    {
        return Err("missing application entry".into());
    }
    let entry = instances[0].tokens[&main];
    // libc's exported errno is authoritative for host adapters as well.
    let mut errno = 0;
    for i in &instances {
        if i.code.manifest.errno != 0 {
            let p = i.global(i.code.manifest.errno)?;
            if errno != 0 && errno != p {
                return Err("multiple errno definitions".into());
            }
            errno = p;
        }
    }
    let owner = Arc::new(Loaded {
        template,
        global_start,
        errno,
        tokens: BTreeMap::new(),
        library: None,
        directory: PathBuf::new(),
        collection_needed,
        modules: instances,
        function_modules,
    });
    Ok(Prepared {
        entry,
        owner,
        functions: functions.into_iter(),
    })
}
