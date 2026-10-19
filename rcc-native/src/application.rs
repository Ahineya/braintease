//! AOT application packages. Host shared-object relocation is delegated to the
//! platform linker; the package exposes only an explicit versioned helper table.
use super::*;
use rcc_ir::native_image::{Function, Helper, Manifest, MAGIC, MAX_IMAGE_BYTES};
use std::{path::Path, process::Command};

pub(super) fn manifest(compiler: &Compiler, cells: u32) -> Result<Manifest, String> {
    let library = compiler
        .dynamic
        .as_ref()
        .is_some_and(|m| !m.name.is_empty());
    let main = compiler.ir.get_function("main");
    if !library && main.is_none_or(|f| f.is_external || f.is_vararg || f.parameters.len() != 1) {
        return Err("application startup main must accept one argument-context pointer".into());
    }
    let mut helpers = BTreeMap::new();
    for (name, _) in &compiler.imports {
        let arity =
            helper_arity(name).ok_or_else(|| format!("unknown application helper {name}"))?;
        helpers.insert(name.clone(), arity);
    }
    // C calls may have a valid source declaration without a standalone IR
    // declaration. callee() records those imports during code generation too.
    for name in compiler
        .functions
        .keys()
        .filter(|name| compiler.ir.get_function(name).is_none_or(|f| f.is_external))
    {
        if let Some((_, arity)) = rcc_ir::target::NATIVE_SERVICES
            .iter()
            .find(|(n, _)| *n == name)
        {
            helpers.insert(format!("rcc_native_ext_{name}"), *arity);
        }
    }
    let (initializers, relocations) = globals::image_data(compiler)?;
    let result = Manifest {
        abi: if compiler.dynamic.is_some() {
            rcc_ir::native_image::MODULE_ABI
        } else {
            rcc_ir::native_image::ABI
        },
        arch: std::env::consts::ARCH.into(),
        os: std::env::consts::OS.into(),
        entry: if library {
            0
        } else {
            compiler.codes["main"] as u32
        },
        global_cells: cells,
        global_bank: compiler.global_bank as u32,
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
        initializers,
        module: compiler.dynamic.clone().map(|mut m| {
            m.relocations = relocations;
            for export in &mut m.exports {
                export.value = if export.function {
                    compiler.codes[&export.name]
                } else {
                    compiler.globals[&export.name]
                };
            }
            m
        }),
    };
    Ok(result)
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
        "rcc_native_image_global" => 1,
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
    write_package(object, manifest.unwrap(), output)
}

fn type_signature(t: &rcc_ir::ir::IrType) -> String {
    use rcc_ir::ir::IrType::*;
    match t {
        FatPtr(_) => "ptr".into(),
        Struct { fields, packed, .. } => format!(
            "struct:{packed}[{}]",
            fields
                .iter()
                .map(type_signature)
                .collect::<Vec<_>>()
                .join(",")
        ),
        Array { size, element_type } => format!("array:{size}:{}", type_signature(element_type)),
        _ => t.to_string(),
    }
}
fn function_signature(f: &rcc_ir::ir::Function) -> String {
    format!(
        "fn({};{})->{}",
        f.parameters
            .iter()
            .map(|(_, t)| type_signature(t))
            .collect::<Vec<_>>()
            .join(","),
        f.is_vararg,
        type_signature(&f.return_type)
    )
}

/// Link against guest library interfaces without incorporating their IR/code.
/// The paths embedded in dependencies name files in the guest filesystem.
pub fn compile_module(
    modules: &[Module],
    output: &Path,
    name: &str,
    exports: &[String],
    dependencies: &[(std::path::PathBuf, String)],
) -> Result<(), String> {
    use rcc_ir::{ir::Linkage as IrLinkage, native_image::*};
    let roots: Vec<String> = if name.is_empty() {
        vec!["main".into()]
    } else if !exports.is_empty() {
        exports.to_vec()
    } else {
        modules
            .iter()
            .flat_map(|m| {
                m.functions
                    .iter()
                    .filter(|f| !f.is_external && !m.internal_functions.contains(&f.name))
                    .map(|f| f.name.clone())
                    .chain(
                        m.globals
                            .iter()
                            .filter(|g| g.linkage == IrLinkage::External)
                            .map(|g| g.name.clone()),
                    )
            })
            .collect()
    };
    if roots.is_empty() {
        return Err("shared library needs exports".into());
    }
    let root_refs: Vec<_> = roots.iter().map(String::as_str).collect();
    let module = rcc_ir::link::link_modules(modules, Target::Native, &root_refs)?;
    let mut metadata = ModuleMetadata {
        name: name.into(),
        dependencies: vec![],
        exports: vec![],
        imports: vec![],
        relocations: vec![],
    };
    if !name.is_empty() {
        for root in &roots {
            let (function, signature) =
                if let Some(f) = module.get_function(root).filter(|f| !f.is_external) {
                    (true, function_signature(f))
                } else if let Some(g) = module
                    .globals
                    .iter()
                    .find(|g| g.name == *root && g.linkage != IrLinkage::Declaration)
                {
                    (false, type_signature(&g.var_type))
                } else {
                    return Err(format!("undefined library export {root}"));
                };
            metadata.exports.push(Symbol {
                name: root.clone(),
                value: 0,
                function,
                signature,
            });
        }
        metadata.exports.sort_by(|a, b| a.name.cmp(&b.name));
        metadata.exports.dedup_by(|a, b| a.name == b.name);
    }
    let references = rcc_ir::link::referenced_symbols(&module);
    let mut calls = BTreeMap::<&str, Vec<(usize, Option<usize>)>>::new();
    for instruction in module
        .functions
        .iter()
        .flat_map(|f| &f.blocks)
        .flat_map(|b| &b.instructions)
    {
        if let rcc_ir::ir::Instruction::Call {
            function: rcc_ir::ir::Value::Global(n) | rcc_ir::ir::Value::Function(n),
            args,
            stack_args_from,
            ..
        } = instruction
        {
            calls
                .entry(n)
                .or_default()
                .push((args.len(), *stack_args_from));
        }
    }
    let mut imported = BTreeSet::new();
    for (path, guest) in dependencies {
        let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let (m, _, _) = decode(&bytes)?;
        let library = m.module.ok_or("dependency is not a shared library")?;
        if library.name.is_empty()
            || m.arch != std::env::consts::ARCH
            || m.os != std::env::consts::OS
        {
            return Err("incompatible shared library".into());
        }
        metadata.dependencies.push(Dependency {
            name: library.name.clone(),
            path: guest.clone(),
            interface: interface(&library),
        });
        for symbol in &library.exports {
            let defined = module
                .get_function(&symbol.name)
                .is_some_and(|f| !f.is_external)
                || module
                    .globals
                    .iter()
                    .any(|g| g.name == symbol.name && g.linkage != IrLinkage::Declaration);
            if !references.contains(&symbol.name) || defined {
                continue;
            }
            if !imported.insert(symbol.name.clone()) {
                return Err(format!("ambiguous shared symbol {}", symbol.name));
            }
            let signature = if let Some(f) = module.get_function(&symbol.name) {
                Some(function_signature(f))
            } else {
                module
                    .globals
                    .iter()
                    .find(|g| g.name == symbol.name)
                    .map(|g| type_signature(&g.var_type))
            };
            if signature.as_ref().is_some_and(|s| *s != symbol.signature) {
                return Err(format!("shared symbol ABI mismatch: {}", symbol.name));
            }
            if symbol.function {
                // C calls need not have a separate IR declaration. Validate
                // their ordinary/variadic argument protocol as well.
                let (parameters, tail) = symbol
                    .signature
                    .strip_prefix("fn(")
                    .and_then(|s| s.split_once(';'))
                    .ok_or("invalid library function signature")?;
                let mut depth = 0;
                let mut fixed = usize::from(!parameters.is_empty());
                for c in parameters.chars() {
                    match c {
                        '[' | '(' => depth += 1,
                        ']' | ')' => depth -= 1,
                        ',' if depth == 0 => fixed += 1,
                        _ => {}
                    }
                }
                let variadic = tail.starts_with("true)");
                for &(arity, packing) in calls.get(symbol.name.as_str()).into_iter().flatten() {
                    if (!variadic && arity != fixed)
                        || (variadic && (arity < fixed || packing != Some(fixed)))
                    {
                        return Err(format!("shared call ABI mismatch: {}", symbol.name));
                    }
                }
            }
            metadata.imports.push(Import {
                local: 0xC000_0000 | (metadata.imports.len() as u64 + 1),
                library: library.name.clone(),
                symbol: symbol.clone(),
            });
        }
    }
    metadata
        .dependencies
        .retain(|d| metadata.imports.iter().any(|i| i.library == d.name));
    let (object, manifest) =
        compile_object_config(&module, &BTreeSet::new(), true, Some(metadata), &roots)?;
    write_package(object, manifest.unwrap(), output)
}

fn write_package(object: Vec<u8>, mut manifest: Manifest, output: &Path) -> Result<(), String> {
    let compact = manifest.abi == rcc_ir::native_image::MODULE_ABI;
    let mut data = if compact {
        vec![0u16; manifest.global_cells as usize]
    } else {
        vec![]
    };
    let mut relocations = Vec::new();
    for &(p, value, width, relocation) in if compact {
        &manifest.initializers[..]
    } else {
        &[]
    } {
        let index = manifest.global_index(p, width)?;
        if relocation {
            relocations.push((p, value, width, true));
        } else {
            for i in 0..width as usize {
                data[index + i] = (value >> (16 * i)) as u16;
            }
        }
    }
    if compact {
        manifest.initializers = relocations;
    }
    while data.last() == Some(&0) {
        data.pop();
    }
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
    bytes.extend_from_slice(&json);
    for cell in data {
        bytes.extend_from_slice(&cell.to_le_bytes());
    }
    bytes.extend_from_slice(&payload);
    if bytes.len() > MAX_IMAGE_BYTES {
        return Err(format!("hosted executable needs {} octets ({} manifest, {} code), exceeds package limit {MAX_IMAGE_BYTES}", bytes.len(), json.len(), payload.len()));
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

#[cfg(test)]
mod tests {
    #[test]
    fn implicit_scalar_service_calls_are_in_the_application_manifest() {
        let module = rcc_ir::parse_module(
            r#"module app {
          target native
          function i64 @main(i32 %0) {
            block 0:
              %1 = call i64 global @__rcc_host_math(0, 0, 0)
              ret %1
          }
        }"#,
        )
        .unwrap();
        let (_, manifest) =
            super::super::compile_object_inner(&module, &Default::default(), true).unwrap();
        let helpers = manifest.unwrap().helpers;
        assert!(helpers
            .iter()
            .any(|h| h.symbol == "rcc_native_ext___rcc_host_math" && h.arity == 3));
    }
}
