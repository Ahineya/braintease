//! Trusted native bootstrap. Bounded child I/O is not a security sandbox.
use super::{
    protocol::{self, WireToken},
    syntax::MAX_BYTES,
};
use crate::{CompileError, Target};
use std::{
    collections::{hash_map::DefaultHasher, HashSet},
    hash::{Hash, Hasher},
    io::{Read, Write},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::mpsc,
    time::{Duration, Instant},
};

pub trait Executor {
    fn execute(
        &mut self,
        package: &Path,
        entry: &str,
        caller: Target,
        input: &[WireToken],
    ) -> Result<Vec<WireToken>, CompileError>;
}
pub struct NativeExecutor;
struct OwnedChild(std::process::Child);
impl std::ops::Deref for OwnedChild {
    type Target = std::process::Child;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
impl std::ops::DerefMut for OwnedChild {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}
impl Drop for OwnedChild {
    fn drop(&mut self) {
        if !matches!(self.0.try_wait(), Ok(Some(_))) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
}
thread_local! { static HOST: std::cell::Cell<usize> = const { std::cell::Cell::new(0) }; }
pub fn in_host() -> bool {
    HOST.with(|n| n.get() != 0)
}
struct Host;
impl Host {
    fn enter() -> Self {
        HOST.with(|n| n.set(n.get() + 1));
        Self
    }
}
impl Drop for Host {
    fn drop(&mut self) {
        HOST.with(|n| n.set(n.get() - 1));
    }
}
fn io(e: std::io::Error) -> CompileError {
    CompileError::plain(format!("macro executor: {e}"))
}
fn literal(text: &str) -> String {
    format!(
        "\"{}\"",
        text.replace('\\', "\\\\")
            .replace('"', "\\\"")
            .replace('\n', "\\n")
            .replace('\r', "\\r")
    )
}

fn inputs(
    path: &Path,
    stack: &mut Vec<PathBuf>,
    seen: &mut HashSet<PathBuf>,
    out: &mut Vec<(PathBuf, String)>,
) -> Result<(), CompileError> {
    let path = super::source::canonical(path).map_err(io)?;
    if stack.contains(&path) {
        return Err(CompileError::plain(
            "macro implementation dependency/bootstrap cycle",
        ));
    }
    if !seen.insert(path.clone()) {
        return Ok(());
    }
    let source = super::source::read(&path).map_err(io)?;
    if source.len() > MAX_BYTES * 16 {
        return Err(CompileError::plain(
            "macro implementation source exceeds limit",
        ));
    }
    let tokens = crate::lexer::tokenize(&source)?;
    let mut ast = crate::parser::parse_for_target(&source, &tokens, Target::Native)?;
    if ast.externs.len() > 0 && !path.ends_with("std/io.rfl") {
        return Err(CompileError::plain(format!(
            "macro deterministic profile forbids extern declarations in {}",
            path.display()
        )));
    }
    super::profile::syntax(&mut ast)?;
    stack.push(path.clone());
    for import in ast.imports.iter().chain(&ast.macro_imports) {
        if import.path == "std/macro_host" {
            return Err(CompileError::plain(
                "macro_host is private executor transport",
            ));
        }
        let dependency = crate::modules::resolve_spec(&path, &import.path)?;
        if dependency.extension().is_some_and(|e| e == "c") {
            return Err(CompileError::plain(
                "macro deterministic profile forbids C imports",
            ));
        }
        inputs(&dependency, stack, seen, out)?;
    }
    stack.pop();
    out.push((path, source));
    Ok(())
}
pub fn validate(package: &Path, entry: &str) -> Result<(), CompileError> {
    executable(package, entry).map(|_| ())
}
pub(super) fn executable(package: &Path, entry: &str) -> Result<PathBuf, CompileError> {
    let source = super::source::read(package).map_err(io)?;
    let tokens = crate::lexer::tokenize(&source)?;
    let ast = crate::parser::parse_for_target(&source, &tokens, Target::Native)?;
    let function = ast
        .functions
        .iter()
        .find(|f| f.name == entry && f.exported && ast.proc_macros.contains(&f.name))
        .ok_or_else(|| {
            CompileError::plain(format!("`{entry}` is not an exported procedural macro"))
        })?;
    if function.is_async
        || function.c_export
        || !function.type_params.is_empty()
        || function.params.len() != 1
        || function.ret.is_none()
    {
        return Err(CompileError::plain(
            "a procedural macro needs the non-generic signature fn(TokenStream) -> TokenStream",
        ));
    }
    let mut dependencies = Vec::new();
    let mut seen = HashSet::new();
    inputs(package, &mut Vec::new(), &mut seen, &mut dependencies)?;
    inputs(
        &Path::new(env!("CARGO_MANIFEST_DIR")).join("std/macro_host.rfl"),
        &mut Vec::new(),
        &mut seen,
        &mut dependencies,
    )?;
    dependencies.sort_by(|a, b| a.0.cmp(&b.0));
    let mut key = String::from("RFL macro bootstrap protocol 2\n");
    key.push_str(entry);
    for (path, source) in &dependencies {
        key.push_str(&format!("\n{}:{}:{source}", path.display(), source.len()));
        let (manifest, declared) = super::inputs::declared(path)?;
        key.push_str(&manifest);
        for dep in declared {
            key.push_str(&format!("{dep:?}"));
        }
    }
    let (manifest, declared) = super::inputs::declared(package)?;
    key.push_str(&manifest);
    for dep in declared {
        key.push_str(&format!("{:?}", dep));
    }
    key.push_str(
        &super::source::read(&Path::new(env!("CARGO_MANIFEST_DIR")).join("std/macro_host.rfl"))
            .map_err(io)?,
    );
    key.push_str(include_str!("../../std/macro.rfl"));
    let compiler = std::env::current_exe().map_err(io)?;
    let meta = std::fs::metadata(compiler).map_err(io)?;
    key.push_str(env!("CARGO_PKG_VERSION"));
    static COMPILER: std::sync::OnceLock<Result<u64, String>> = std::sync::OnceLock::new();
    let identity = COMPILER
        .get_or_init(|| {
            let path = std::env::current_exe().map_err(|e| e.to_string())?;
            let bytes = std::fs::read(path).map_err(|e| e.to_string())?;
            let mut hash = DefaultHasher::new();
            bytes.hash(&mut hash);
            Ok(hash.finish())
        })
        .as_ref()
        .map_err(|e| CompileError::plain(e.clone()))?;
    key.push_str(&format!(
        "{identity:016x}:{:?}:{}",
        meta.modified(),
        meta.len()
    ));
    let runtime = rcc_native::runtime_library();
    if let Ok(meta) = std::fs::metadata(runtime.map_err(CompileError::plain)?) {
        key.push_str(&format!("{:?}:{}", meta.modified(), meta.len()));
    }
    let mut hash = DefaultHasher::new();
    key.hash(&mut hash);
    let directory = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../target/rfl-macros")
        .join(format!("{:016x}", hash.finish()));
    std::fs::create_dir_all(&directory).map_err(io)?;
    // One artifact may be requested by several compiler/LSP processes. The OS
    // releases this lock if its owner exits, including after a failed compile.
    let lock = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .open(directory.join("lock"))
        .map_err(io)?;
    let started = Instant::now();
    loop {
        match lock.try_lock() {
            Ok(()) => break,
            Err(std::fs::TryLockError::WouldBlock) => {
                if started.elapsed() > Duration::from_secs(30) {
                    return Err(CompileError::plain(
                        "macro artifact lock wait exceeded 30 seconds",
                    ));
                }
                std::thread::sleep(Duration::from_millis(20));
            }
            Err(std::fs::TryLockError::Error(e)) => return Err(io(e)),
        }
    }
    let executable = directory.join("macro");
    let stamp = directory.join("inputs");
    // Full key comparison makes hash collisions cache misses, not wrong code.
    if executable.is_file() && std::fs::read_to_string(&stamp).ok().as_deref() == Some(&key) {
        return Ok(executable);
    }
    let source = directory.join("entry.rfl");
    let wrapper=format!("import {{TokenStream}} from \"std/macro\";\nimport {{read_request,write_response}} from \"std/macro_host\";\nimport {{{entry} as macro_entry}} from {};\nfn macro_dispatch(input:TokenStream)->TokenStream{{macro_entry(input)}}\nfn main(){{let input=read_request();if(input.target==\"invalid protocol\"){{write_response(input);}}else{{write_response(macro_dispatch(input));}}}}\n",literal(&package.to_string_lossy()));
    std::fs::write(&source, wrapper).map_err(io)?;
    let _host = Host::enter();
    let (typed, cmods) = crate::modules::check_path_for_target(&source, Target::Native)?;
    if !cmods.is_empty() {
        return Err(CompileError::plain(
            "macro deterministic profile forbids C imports",
        ));
    }
    super::profile::validate(&typed)?;
    let mut module = crate::lower::lower("macro_host", &typed)?;
    module.target = Target::Native;
    rcc_native::compile_executable(&module, &executable).map_err(CompileError::plain)?;
    let pending = directory.join("inputs.pending");
    std::fs::write(&pending, key).map_err(io)?;
    std::fs::rename(pending, stamp).map_err(io)?;
    Ok(executable)
}
impl Executor for NativeExecutor {
    fn execute(
        &mut self,
        package: &Path,
        entry: &str,
        caller: Target,
        input: &[WireToken],
    ) -> Result<Vec<WireToken>, CompileError> {
        let executable = executable(package, entry)?;
        let (_, dependencies) = super::inputs::declared(package)?;
        let input = protocol::request(caller.name(), input, &dependencies)?;
        let mut child = OwnedChild(
            Command::new(executable)
                .stdin(Stdio::piped())
                .stdout(Stdio::piped())
                .stderr(Stdio::piped())
                .env("RCC_NATIVE_TIMEOUT", "5")
                .spawn()
                .map_err(io)?,
        );
        let mut stdin = child.stdin.take().unwrap();
        let writer = std::thread::spawn(move || stdin.write_all(&input));
        let (tx, rx) = mpsc::channel();
        let threads = [
            child
                .stdout
                .take()
                .map(|s| Box::new(s) as Box<dyn Read + Send>),
            child
                .stderr
                .take()
                .map(|s| Box::new(s) as Box<dyn Read + Send>),
        ]
        .into_iter()
        .enumerate()
        .map(|(i, s)| {
            let tx = tx.clone();
            std::thread::spawn(move || {
                let mut s = s.unwrap();
                let mut bytes = Vec::new();
                let mut chunk = [0; 4096];
                loop {
                    match s.read(&mut chunk) {
                        Ok(0) => break,
                        Ok(n) => {
                            bytes.extend_from_slice(&chunk[..n]);
                            if bytes.len() > protocol::MAX_FRAME + 32 {
                                let _ = tx.send(Err("macro output limit exceeded"));
                                return;
                            }
                        }
                        Err(_) => {
                            let _ = tx.send(Err("macro output read failed"));
                            return;
                        }
                    }
                }
                let _ = tx.send(Ok((i, bytes)));
            })
        })
        .collect::<Vec<_>>();
        drop(tx);
        let start = Instant::now();
        let mut output = [Vec::new(), Vec::new()];
        let mut received = 0;
        let mut failure = None;
        while received < 2 {
            match rx.recv_timeout(Duration::from_millis(10)) {
                Ok(Ok((i, bytes))) => {
                    output[i] = bytes;
                    received += 1;
                }
                Ok(Err(e)) => {
                    failure = Some(e);
                    break;
                }
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    failure = Some("macro output channel closed");
                    break;
                }
                Err(mpsc::RecvTimeoutError::Timeout) => {}
            }
            if start.elapsed() > Duration::from_secs(7) {
                failure = Some("macro execution wall-time limit exceeded");
                break;
            }
        }
        if failure.is_some() {
            let _ = child.kill();
        }
        let status = child.wait().map_err(io)?;
        let write = writer
            .join()
            .map_err(|_| CompileError::plain("macro input worker failed"))?;
        for thread in threads {
            let _ = thread.join();
        }
        if let Some(e) = failure {
            return Err(CompileError::plain(e));
        }
        write.map_err(io)?;
        if !status.success() {
            return Err(CompileError::plain(format!(
                "macro executable failed ({status}): {}",
                String::from_utf8_lossy(&output[1])
            )));
        }
        protocol::response(&output[0])
    }
}
