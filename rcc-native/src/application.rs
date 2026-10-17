//! AOT application packages. Host shared-object relocation is delegated to the
//! platform linker; the package exposes only an explicit versioned helper table.
use super::*;
use rcc_ir::native_image::{Function, Helper, Manifest, ABI, MAGIC, MAX_IMAGE_BYTES};
use std::{path::Path, process::Command};

pub(super) fn manifest(compiler: &Compiler, cells: u32) -> Result<Manifest, String> {
    let main = compiler
        .ir
        .get_function("main")
        .ok_or("application needs its startup main")?;
    if main.is_external || main.is_vararg || main.parameters.len() != 1 {
        return Err("application startup main must accept one argument-context pointer".into());
    }
    let mut helpers = BTreeMap::new();
    for (name, _) in &compiler.imports {
        let arity =
            helper_arity(name).ok_or_else(|| format!("unknown application helper {name}"))?;
        helpers.insert(name.clone(), arity);
    }
    for f in compiler.ir.functions.iter().filter(|f| f.is_external) {
        if let Some((_, arity)) = rcc_ir::target::NATIVE_SERVICES
            .iter()
            .find(|(n, _)| *n == f.name)
        {
            helpers.insert(format!("rcc_native_ext_{}", f.name), *arity);
        }
    }
    Ok(Manifest {
        abi: ABI,
        arch: std::env::consts::ARCH.into(),
        os: std::env::consts::OS.into(),
        entry: compiler.codes["main"] as u32,
        global_cells: cells,
        errno: compiler.globals.get("errno").copied().unwrap_or(0),
        functions: compiler
            .ir
            .functions
            .iter()
            .filter(|f| !f.is_external)
            .map(|f| Function {
                token: compiler.codes[&f.name] as u32,
                symbol: format!("rcc_user_{}", f.name),
                arity: (f.parameters.len() + usize::from(f.is_vararg)) as u32,
            })
            .collect(),
        helpers: helpers
            .into_iter()
            .map(|(symbol, arity)| Helper { symbol, arity })
            .collect(),
        initializers: globals::image_initializers(compiler)?,
    })
}
fn helper_arity(name: &str) -> Option<usize> {
    Some(match name {
        "rcc_native_poll"
        | "rcc_native_process_checkpoint"
        | "rcc_native_stack_mark"
        | "rcc_native_process_pc"
        | "rcc_native_process_bad_pc" => 0,
        "rcc_native_stack_alloc"
        | "rcc_native_stack_restore"
        | "rcc_native_process_frame"
        | "rcc_native_process_slot"
        | "rcc_native_process_set_pc"
        | "rcc_native_process_arg"
        | "rcc_native_process_return"
        | "rcc_native_image_token" => 1,
        "rcc_native_load"
        | "rcc_native_gep"
        | "rcc_native_register"
        | "rcc_native_function"
        | "rcc_native_process_save"
        | "rcc_native_process_argument" => 2,
        "rcc_native_store" | "rcc_native_process_call" => 3,
        "rcc_native_process_request" => 4,
        _ => return None,
    })
}

pub fn compile_application(modules: &[Module], output: &Path) -> Result<(), String> {
    let module = link_modules(modules)?;
    let (object, manifest) = compile_object_inner(&module, &BTreeSet::new(), true)?;
    let manifest = manifest.unwrap();
    let parent = output
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    let nonce = format!(
        "{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    );
    let directory = parent.join(format!(".fuse-x-link-{nonce}"));
    std::fs::create_dir(&directory).map_err(|e| e.to_string())?;
    struct Cleanup(std::path::PathBuf);
    impl Drop for Cleanup {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    let _cleanup = Cleanup(directory.clone());
    let object_path = directory.join("program.o");
    let glue = directory.join("bindings.c");
    let library = directory.join("program.dylib");
    std::fs::write(&object_path, object).map_err(|e| e.to_string())?;
    let mut source = String::from("typedef unsigned long long U;\nstatic U helpers[");
    source.push_str(&manifest.helpers.len().max(1).to_string());
    source
        .push_str("];\n__attribute__((visibility(\"default\"))) void fuse_x_bind(const U *p) {\n");
    for i in 0..manifest.helpers.len() {
        source.push_str(&format!("helpers[{i}]=p[{i}];\n"));
    }
    source.push_str("}\n");
    for (i, helper) in manifest.helpers.iter().enumerate() {
        let args = (0..=helper.arity)
            .map(|n| format!("U a{n}"))
            .collect::<Vec<_>>()
            .join(",");
        let types = vec!["U"; helper.arity + 1].join(",");
        let values = (0..=helper.arity)
            .map(|n| format!("a{n}"))
            .collect::<Vec<_>>()
            .join(",");
        source.push_str(&format!("__attribute__((visibility(\"hidden\"))) U {}({args}) {{return ((U (*)({types}))helpers[{i}])({values});}}\n",helper.symbol));
    }
    std::fs::write(&glue, source).map_err(|e| e.to_string())?;
    let mut command =
        Command::new(std::env::var_os("RCC_NATIVE_CC").unwrap_or_else(|| "cc".into()));
    command
        .args(["-O2", "-fPIC"])
        .arg(&glue)
        .arg(&object_path)
        .arg("-o")
        .arg(&library);
    if cfg!(target_os = "macos") {
        let sdk = Command::new("xcrun")
            .args(["--sdk", "macosx", "--show-sdk-path"])
            .output()
            .map_err(|e| e.to_string())?;
        if !sdk.status.success() {
            return Err(String::from_utf8_lossy(&sdk.stderr).into());
        }
        command
            .args(["-dynamiclib", "-isysroot"])
            .arg(String::from_utf8_lossy(&sdk.stdout).trim());
    } else if cfg!(target_os = "linux") {
        command.args(["-shared", "-Wl,-Bsymbolic"]);
    } else {
        return Err("hosted application linking supports macOS and Linux".into());
    }
    let result = command.output().map_err(|e| e.to_string())?;
    if !result.status.success() {
        return Err(format!(
            "application link failed: {}",
            String::from_utf8_lossy(&result.stderr)
        ));
    }
    let json = serde_json::to_vec(&manifest).map_err(|e| e.to_string())?;
    let payload = std::fs::read(&library).map_err(|e| e.to_string())?;
    let mut bytes = MAGIC.to_vec();
    bytes.extend_from_slice(&(json.len() as u32).to_le_bytes());
    bytes.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    bytes.extend(json);
    bytes.extend(payload);
    if bytes.len() > MAX_IMAGE_BYTES {
        return Err("hosted executable exceeds package size limit".into());
    }
    let temporary = directory.join("executable");
    std::fs::write(&temporary, bytes).map_err(|e| e.to_string())?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&temporary, std::fs::Permissions::from_mode(0o755))
            .map_err(|e| e.to_string())?;
    }
    std::fs::rename(&temporary, output).map_err(|e| e.to_string())
}
