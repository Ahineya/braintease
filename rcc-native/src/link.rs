use rcc_ir::ir::Module;
use std::path::{Path, PathBuf};
use std::process::Command;

pub fn runtime_library() -> Result<PathBuf, String> {
    if let Some(path) = std::env::var_os("RCC_NATIVE_RUNTIME") {
        let path = PathBuf::from(path);
        if path.is_file() {
            return Ok(path);
        }
        return Err(format!(
            "RCC_NATIVE_RUNTIME does not exist: {}",
            path.display()
        ));
    }
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..");
    let path = root.join("target/release/librcc_native_runtime.a");
    if path.is_file() {
        Ok(path)
    } else {
        Err(
            "native runtime is missing; build it with cargo build --release -p rcc-native-runtime"
                .into(),
        )
    }
}

pub fn compile_executable(module: &Module, output: &Path) -> Result<(), String> {
    let bytes = super::compile_object(module)?;
    link_object(bytes, output)
}

/// Link kernel units with shared globals and ordinary private application units.
/// Native continuations provide context mechanics; the kernel owns scheduling.
pub fn compile_hosted_executable(
    kernel: &[Module],
    applications: &[Module],
    output: &Path,
) -> Result<(), String> {
    let (module, shared) = hosted_module(kernel, applications)?;
    let bytes = super::compile_object_with_kernel(&module, &shared)?;
    link_object(bytes, output)
}

pub(super) fn hosted_module(
    kernel: &[Module],
    applications: &[Module],
) -> Result<(Module, std::collections::BTreeSet<String>), String> {
    use rcc_ir::ir::Linkage;
    let mut shared = std::collections::BTreeSet::new();
    for (index, module) in kernel.iter().enumerate() {
        for global in &module.globals {
            if global.linkage != Linkage::Declaration {
                shared.insert(
                    if matches!(global.linkage, Linkage::Internal | Linkage::Private) {
                        format!("__unit{index}_{}", global.name)
                    } else {
                        global.name.clone()
                    },
                );
            }
        }
    }
    let modules: Vec<_> = kernel.iter().chain(applications).cloned().collect();
    let module = super::link_modules(&modules)?;
    Ok((module, shared))
}

fn link_object(bytes: Vec<u8>, output: &Path) -> Result<(), String> {
    let runtime = runtime_library()?;
    let parent = output
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    let nonce = format!(
        "{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_err(|e| e.to_string())?
            .as_nanos()
    );
    let object = parent.join(format!(".rcc-native-{nonce}.o"));
    let executable = parent.join(format!(".rcc-native-{nonce}.exe"));
    let _cleanup = Artifacts(vec![object.clone(), executable.clone()]);
    std::fs::write(&object, bytes).map_err(|e| e.to_string())?;
    let mut command =
        Command::new(std::env::var_os("RCC_NATIVE_CC").unwrap_or_else(|| "cc".into()));
    command
        .arg(&object)
        .arg(&runtime)
        .arg("-o")
        .arg(&executable);
    // Optional host platform libraries. Each name is one literal linker
    // argument; this is not a shell command or a route to a host C ABI call.
    if let Ok(libraries) = std::env::var("RCC_NATIVE_LIBS") {
        for name in libraries.split(',').filter(|s| !s.is_empty()) {
            if !name
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
            {
                return Err("RCC_NATIVE_LIBS must contain comma-separated library names".into());
            }
            command.arg(format!("-l{name}"));
        }
    }
    if cfg!(target_os = "macos") {
        // Match the selected developer toolchain's SDK. The driver's implicit
        // SDK can point at unrelated CommandLineTools installations.
        let sdk = Command::new("xcrun")
            .args(["--sdk", "macosx", "--show-sdk-path"])
            .output()
            .map_err(|e| format!("cannot locate macOS SDK: {e}"))?;
        if !sdk.status.success() {
            return Err(format!(
                "cannot locate macOS SDK: {}",
                String::from_utf8_lossy(&sdk.stderr)
            ));
        }
        let sdk = String::from_utf8(sdk.stdout).map_err(|e| e.to_string())?;
        command
            .arg("-isysroot")
            .arg(sdk.trim())
            .arg("-Wl,-dead_strip");
        for framework in [
            "CoreFoundation",
            "CoreGraphics",
            "CoreText",
            "CoreAudio",
            "AudioToolbox",
            "AppKit",
            "QuartzCore",
            "Metal",
            "Foundation",
            "IOKit",
            "CoreVideo",
        ] {
            command.args(["-framework", framework]);
        }
        command.args(["-liconv", "-lSystem"]);
    } else if cfg!(target_os = "linux") {
        command.args(["-ldl", "-lpthread", "-lm", "-lasound"]);
    } else {
        return Err("native executable linking currently supports macOS and Linux".into());
    }
    let result = command
        .output()
        .map_err(|e| format!("cannot run native linker: {e}"))?;
    if !result.status.success() {
        return Err(format!(
            "native link failed:\n{}",
            String::from_utf8_lossy(&result.stderr)
        ));
    }
    std::fs::rename(&executable, output)
        .map_err(|e| format!("cannot publish native executable {}: {e}", output.display()))?;
    Ok(())
}
struct Artifacts(Vec<PathBuf>);
impl Drop for Artifacts {
    fn drop(&mut self) {
        for path in &self.0 {
            let _ = std::fs::remove_file(path);
        }
    }
}
