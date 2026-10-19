//! Ahead-of-time native code generation from shared RCC/RFL IR.
//!
//! V1 executes host machine code with a shared RCC/RFL compatibility ABI:
//! a hidden runtime context and one u64 carrier per scalar. Carriers preserve
//! the declared integer width or IEEE bits; they are not host C declarations.
mod application;
mod arithmetic;
mod emit;
mod entry;
mod floating;
mod globals;
mod instruction;
mod link;
mod liveness;
mod process;
mod suspension;
mod units;
pub use application::compile_application;
pub use units::link_modules;

use cranelift_codegen::ir::{types, AbiParam, Signature};
use cranelift_codegen::settings::{self, Configurable};
use cranelift_module::{FuncId, Linkage, Module as ClModule};
use cranelift_object::{ObjectBuilder, ObjectModule};
pub use link::{compile_executable, compile_hosted_executable, runtime_library};
use rcc_ir::{ir::Module, target::Target};
use std::collections::{BTreeMap, BTreeSet};

struct Compiler {
    object: ObjectModule,
    functions: BTreeMap<String, FuncId>,
    direct_functions: BTreeMap<String, FuncId>,
    imports: BTreeMap<String, FuncId>,
    codes: BTreeMap<String, u64>,
    globals: BTreeMap<String, u64>,
    ir: Module,
    address_taken: BTreeSet<String>,
    resumable: bool,
    application: bool,
    global_bank: u64,
    global_cells: u64,
}

impl Compiler {
    fn signature(&self, arguments: usize) -> Signature {
        let mut signature = self.object.make_signature();
        signature.params = vec![AbiParam::new(types::I64); arguments + 1];
        signature.returns.push(AbiParam::new(types::I64));
        signature
    }
    fn callee(&mut self, name: &str, arguments: usize) -> Result<FuncId, String> {
        if let Some(function) = self.ir.get_function(name) {
            if (function.is_vararg && arguments < function.parameters.len())
                || (!function.is_vararg && function.parameters.len() != arguments)
            {
                return Err(format!("native call {name} argument count mismatch"));
            }
            if !function.is_external {
                return Ok(self
                    .direct_functions
                    .get(name)
                    .copied()
                    .unwrap_or(self.functions[name]));
            }
        }
        if self.application && !rcc_ir::native_image::application_service(name) {
            return Err(format!(
                "application external {name} requires a guest libc definition"
            ));
        }
        let arity = rcc_ir::target::NATIVE_SERVICES
            .iter()
            .find(|(symbol, _)| *symbol == name)
            .map(|(_, arity)| *arity)
            .ok_or_else(|| format!("native external {name} has no compatible runtime binding"))?;
        if arity != arguments {
            return Err(format!(
                "native runtime binding {name} expects {arity} arguments, got {arguments}"
            ));
        }
        if let Some(id) = self.functions.get(name) {
            return Ok(*id);
        }
        let id = self
            .object
            .declare_function(
                &format!("rcc_native_ext_{name}"),
                Linkage::Import,
                &self.signature(arity),
            )
            .map_err(|e| e.to_string())?;
        self.functions.insert(name.into(), id);
        Ok(id)
    }
    fn function_token(&mut self, name: &str) -> Result<u64, String> {
        if self.resumable && self.ir.get_function(name).is_none_or(|f| f.is_external) {
            return Err(format!(
                "resumable native code cannot take the address of runtime binding {name}"
            ));
        }
        let arity = self
            .ir
            .get_function(name)
            .map(|f| f.parameters.len())
            .or_else(|| {
                rcc_ir::target::NATIVE_SERVICES
                    .iter()
                    .find(|(n, _)| *n == name)
                    .map(|(_, arity)| *arity)
            })
            .ok_or_else(|| format!("unknown native function pointer {name}"))?;
        self.callee(name, arity)?;
        if !self.codes.contains_key(name) {
            let code = 0x8000_0000 | (self.codes.len() as u64 + 1);
            self.codes.insert(name.into(), code);
        }
        self.address_taken.insert(name.into());
        Ok(self.codes[name])
    }
    fn import(&mut self, name: &str, arguments: usize) -> Result<FuncId, String> {
        if let Some(id) = self.imports.get(name) {
            return Ok(*id);
        }
        let id = self
            .object
            .declare_function(name, Linkage::Import, &self.signature(arguments))
            .map_err(|e| format!("native import {name}: {e}"))?;
        self.imports.insert(name.into(), id);
        Ok(id)
    }
}

fn progress(message: impl std::fmt::Display) {
    if std::env::var_os("RCC_NATIVE_PROGRESS").is_some() {
        eprintln!("rcc-native: {message}");
    }
}

pub fn compile_object(module: &Module) -> Result<Vec<u8>, String> {
    compile_object_inner(module, &BTreeSet::new(), false).map(|(object, _)| object)
}

fn compile_object_with_kernel(
    module: &Module,
    kernel: &BTreeSet<String>,
) -> Result<Vec<u8>, String> {
    compile_object_inner(module, kernel, false).map(|(object, _)| object)
}

fn compile_object_inner(
    module: &Module,
    kernel: &BTreeSet<String>,
    application: bool,
) -> Result<(Vec<u8>, Option<rcc_ir::native_image::Manifest>), String> {
    if module.target != Target::Native {
        return Err("native backend requires IR compiled for --target native".into());
    }
    if !cfg!(any(target_arch = "aarch64", target_arch = "x86_64")) {
        return Err("native backend currently supports 64-bit AArch64/x86-64 hosts".into());
    }
    let mut flags = settings::builder();
    flags.set("opt_level", "speed").map_err(|e| e.to_string())?;
    flags.set("is_pic", "true").map_err(|e| e.to_string())?;
    let isa = cranelift_native::builder()
        .map_err(|e| e.to_string())?
        .finish(settings::Flags::new(flags))
        .map_err(|e| e.to_string())?;
    let builder = ObjectBuilder::new(
        isa,
        module.name.as_bytes(),
        cranelift_module::default_libcall_names(),
    )
    .map_err(|e| e.to_string())?;
    let started = std::time::Instant::now();
    progress("Preparing reachable IR");
    let mut ir = module.clone();
    units::retain_reachable(&mut ir);
    units::hoist_static_allocas(&mut ir);
    progress("Optimizing IR");
    rcc_backend::optimize::optimize_module(&mut ir);
    progress(format_args!(
        "IR optimized ({:.2}s); generating native functions",
        started.elapsed().as_secs_f64()
    ));
    units::retain_reachable(&mut ir);
    let resumable = application || process::enabled(&ir);
    if application {
        for f in ir.functions.iter().filter(|f| f.is_external) {
            if f.name == "exit"
                || ((f.name.starts_with("rcc_process_") || f.name.starts_with("rcc_hosted_"))
                    && f.name != "rcc_hosted_syscall")
            {
                return Err(format!(
                    "application external {} must enter the kernel through guest libc",
                    f.name
                ));
            }
        }
    }
    if !kernel.is_empty() && !process::hosted(&ir) {
        return Err("shared kernel modules require the hosted continuation API".into());
    }
    if resumable {
        if let Some(f) = ir
            .functions
            .iter()
            .find(|f| !f.is_external && f.name.starts_with("rcc_process_"))
        {
            return Err(format!(
                "{} is reserved for native process services",
                f.name
            ));
        }
    }
    let direct = if resumable {
        suspension::direct_functions(&ir)
    } else {
        BTreeSet::new()
    };
    let mut compiler = Compiler {
        object: ObjectModule::new(builder),
        functions: BTreeMap::new(),
        direct_functions: BTreeMap::new(),
        imports: BTreeMap::new(),
        codes: BTreeMap::new(),
        globals: BTreeMap::new(),
        ir,
        address_taken: BTreeSet::new(),
        resumable,
        application,
        global_bank: 1,
        global_cells: 1,
    };
    let global_cells = compiler
        .ir
        .globals
        .iter()
        .filter(|g| !kernel.contains(&g.name))
        .try_fold(1u64, |size, g| {
            size.checked_add(Target::Native.layout().size_in_cells(&g.var_type)?)
                .ok_or_else(|| "native globals size overflow".to_string())
        })?;
    let bank_cells = u64::from(Target::Native.layout().bank_cells);
    if global_cells > bank_cells {
        compiler.global_bank = u64::from(rcc_ir::native_image::GLOBAL_BANK);
    }
    compiler.global_cells = global_cells;
    let mut offset = 1u64;
    let mut kernel_offset = 1u64;
    for global in &compiler.ir.globals {
        if matches!(global.linkage, rcc_ir::ir::Linkage::Declaration) {
            return Err(format!(
                "native external global {} needs an IR definition",
                global.name
            ));
        }
        let (bank, cursor) = if kernel.contains(&global.name) {
            (240u64, &mut kernel_offset)
        } else {
            (compiler.global_bank, &mut offset)
        };
        compiler.globals.insert(
            global.name.clone(),
            ((bank + *cursor / bank_cells) << 16) | (*cursor % bank_cells),
        );
        *cursor = cursor
            .checked_add(Target::Native.layout().size_in_cells(&global.var_type)?)
            .ok_or("native globals size overflow")?;
    }
    if offset > bank_cells * u64::from(rcc_ir::native_image::GLOBAL_BANKS)
        || kernel_offset >= u64::from(Target::Native.layout().bank_cells)
    {
        return Err(format!("native global capacity exceeded: {offset} application cells, {kernel_offset} kernel cells"));
    }
    for function in &compiler.ir.functions {
        let name = if function.is_external {
            format!("rcc_native_ext_{}", function.name)
        } else {
            format!("rcc_user_{}", function.name)
        };
        let id = compiler
            .object
            .declare_function(
                &name,
                if function.is_external {
                    Linkage::Import
                } else {
                    Linkage::Export
                },
                &compiler.signature(if resumable && !function.is_external {
                    0
                } else {
                    function.parameters.len() + usize::from(function.is_vararg)
                }),
            )
            .map_err(|e| format!("native declaration {}: {e}", function.name))?;
        if compiler
            .functions
            .insert(function.name.clone(), id)
            .is_some()
        {
            return Err(format!("duplicate native function {}", function.name));
        }
        let code = 0x8000_0000 | (compiler.codes.len() as u64 + 1);
        compiler.codes.insert(function.name.clone(), code);
    }
    for name in direct {
        let function = compiler.ir.get_function(&name).unwrap();
        let signature =
            compiler.signature(function.parameters.len() + usize::from(function.is_vararg));
        let id = compiler
            .object
            .declare_function(&format!("rcc_direct_{name}"), Linkage::Local, &signature)
            .map_err(|e| format!("native direct declaration {name}: {e}"))?;
        compiler.direct_functions.insert(name, id);
    }
    for (name, args) in [
        ("rcc_native_poll", 0),
        ("rcc_native_load", 2),
        ("rcc_native_store", 3),
        ("rcc_native_gep", 2),
        ("rcc_native_stack_mark", 0),
        ("rcc_native_stack_alloc", 1),
        ("rcc_native_stack_restore", 1),
        ("rcc_native_register", 2),
        ("rcc_native_function", 2),
    ] {
        compiler.import(name, args)?;
    }
    let functions = compiler.ir.functions.clone();
    let mut reported = std::time::Instant::now();
    for (index, function) in functions.iter().enumerate() {
        if !function.is_external {
            if reported.elapsed().as_secs() >= 5 {
                progress(format_args!(
                    "Generating function {}/{}: {}",
                    index + 1,
                    functions.len(),
                    function.name
                ));
                reported = std::time::Instant::now();
            }
            emit::emit_function(&mut compiler, function)?;
            if compiler.direct_functions.contains_key(&function.name) {
                emit::emit_direct_function(&mut compiler, function)?;
            }
        }
    }
    progress(format_args!(
        "Native functions generated ({:.2}s)",
        started.elapsed().as_secs_f64()
    ));
    let globals = compiler.ir.globals.clone();
    for global in &globals {
        if let Some(value) = &global.initializer {
            globals::register_functions(&mut compiler, value)?;
        }
    }
    let manifest = if application {
        Some(application::manifest(&compiler, offset as u32)?)
    } else {
        entry::emit_entry(&mut compiler, kernel_offset - 1)?;
        None
    };
    let object = compiler
        .object
        .finish()
        .emit()
        .map_err(|e| format!("native object emission: {e}"))?;
    Ok((object, manifest))
}
