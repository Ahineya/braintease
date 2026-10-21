//! Explicit cross-target object emission, without a host linker or runtime.
use std::{env, path::PathBuf};

fn main() {
    if let Err(error) = run() {
        eprintln!("rcc-native-object: {error}");
        std::process::exit(1);
    }
}

fn run() -> Result<(), String> {
    let mut args = env::args_os().skip(1);
    let mut output = None;
    let mut triple = None;
    let mut manifest = None;
    let mut kernel = Vec::new();
    let mut applications = Vec::new();
    while let Some(arg) = args.next() {
        if arg == "--help" || arg == "-h" {
            println!("Usage: rcc-native-object --triple x86_64-unknown-none [--kernel KERNEL.ir | --application MANIFEST.json] INPUT.ir ... -o OUTPUT.o");
            return Ok(());
        }
        if arg == "--triple" {
            triple = Some(
                args.next()
                    .ok_or("--triple requires a value")?
                    .into_string()
                    .map_err(|_| "target triple must be UTF-8")?,
            );
        } else if arg == "--application" {
            manifest = Some(PathBuf::from(
                args.next()
                    .ok_or("--application requires a manifest path")?,
            ));
        } else if arg == "-o" {
            output = Some(PathBuf::from(args.next().ok_or("-o requires a path")?));
        } else {
            let is_kernel = arg == "--kernel";
            if !is_kernel && arg.to_string_lossy().starts_with('-') {
                return Err(format!("unknown option {}", arg.to_string_lossy()));
            }
            let path = PathBuf::from(if is_kernel {
                args.next().ok_or("--kernel requires an IR path")?
            } else {
                arg
            });
            let text =
                std::fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
            let module =
                rcc_ir::parse_module(&text).map_err(|e| format!("{}: {e}", path.display()))?;
            if is_kernel {
                kernel.push(module);
            } else {
                applications.push(module);
            }
        }
    }
    let triple = triple.ok_or("--triple is required")?;
    let output = output.ok_or("-o is required")?;
    if applications.is_empty() && kernel.is_empty() {
        return Err("at least one IR input is required".into());
    }
    if manifest.is_some() && !kernel.is_empty() {
        return Err("--application cannot be combined with --kernel".into());
    }
    let bytes = if let Some(path) = manifest {
        let module = rcc_native::link_modules(&applications)?;
        let (object, manifest) = rcc_native::compile_freestanding_application(&module, &triple)?;
        let json = serde_json::to_vec_pretty(&manifest).map_err(|e| e.to_string())?;
        std::fs::write(&path, json).map_err(|e| format!("{}: {e}", path.display()))?;
        object
    } else if kernel.is_empty() {
        let module = rcc_native::link_modules(&applications)?;
        rcc_native::compile_freestanding_object(&module, &triple)?
    } else {
        rcc_native::compile_freestanding_kernel(&kernel, &applications, &triple)?
    };
    std::fs::write(&output, bytes).map_err(|e| format!("{}: {e}", output.display()))
}
