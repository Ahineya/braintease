//! Direct WebAssembly generation from shared RCC/RFL IR with a C16 runtime.
mod arithmetic;
mod browser;
mod calls;
mod cfg;
mod emit;
mod floating;
mod globals;
mod memory;
mod memory_emit;
mod runtime;
mod services;
mod verify;

use rcc_ir::{ir::*, target::Target};
use std::{
    borrow::Cow,
    collections::{BTreeMap, BTreeSet},
};
use wasm_encoder::*;

/// Export names are also link roots. Browser libraries need not define `main`.
#[derive(Debug, Clone)]
pub struct Options {
    pub exports: Vec<String>,
}
impl Default for Options {
    fn default() -> Self {
        Self {
            exports: vec!["main".into()],
        }
    }
}
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct Statistics {
    pub functions: usize,
    pub structured_functions: usize,
    pub dispatch_functions: usize,
}
#[derive(Debug)]
pub struct Artifact {
    pub bytes: Vec<u8>,
    pub statistics: Statistics,
}

/// Private ABI: i64 bit-pattern carriers, checked function tokens and C16 data.
/// Host imports use typed wrappers; there is no hidden host context or VM loop.
pub fn compile(modules: &[rcc_ir::ir::Module], options: &Options) -> Result<Artifact, String> {
    let mut unique = BTreeSet::new();
    if options.exports.is_empty() {
        return Err("Wasm requires at least one explicit export root".into());
    }
    for module in modules {
        if module
            .functions
            .iter()
            .any(|f| f.name.starts_with("__ripple_") || f.name.starts_with("__wasm_"))
            || module
                .globals
                .iter()
                .any(|g| g.name.starts_with("__ripple_") || g.name.starts_with("__wasm_"))
        {
            return Err("__ripple_ and __wasm_ names are reserved for the Wasm runtime".into());
        }
    }
    for name in &options.exports {
        if !unique.insert(name) {
            return Err(format!("duplicate Wasm export {name:?}"));
        }
        if name == "memory" || name.starts_with("__ripple_") {
            return Err(format!("reserved Wasm export {name}"));
        }
        if !modules.iter().any(|m| {
            m.functions
                .iter()
                .any(|f| f.name == *name && !f.is_external)
                && !m.internal_functions.contains(name)
        }) {
            return Err(format!("Wasm export {name:?} has no public definition"));
        }
    }
    let mut roots: Vec<_> = options.exports.iter().map(String::as_str).collect();
    let definitions: BTreeSet<_> = modules
        .iter()
        .flat_map(|module| {
            module
                .functions
                .iter()
                .filter(|f| !f.is_external && !module.internal_functions.contains(&f.name))
                .map(|f| f.name.as_str())
                .chain(
                    module
                        .globals
                        .iter()
                        .filter(|g| g.linkage == rcc_ir::ir::Linkage::External)
                        .map(|g| g.name.as_str()),
                )
        })
        .collect();
    let mut units = modules.to_vec();
    let reachable = rcc_ir::link::link_modules(modules, Target::Wasm, &roots)?;
    // A live mount call opts into the browser ABI. Its dispatcher/disposal
    // hooks are roots even though no ordinary application call reaches them.
    let browser = reachable.functions.iter().any(|f| {
        f.name == "ripple_browser_mount"
            || rcc_backend::optimize::function_symbols(f).contains("ripple_browser_mount")
    });
    if browser {
        roots.extend(browser::ROOTS);
    }
    let effects = browser
        && reachable.functions.iter().any(|f| {
            f.name == "ripple_browser_effects"
                || rcc_backend::optimize::function_symbols(f).contains("ripple_browser_effects")
        });
    if effects {
        roots.extend(browser::EFFECT_ROOTS);
    }
    let executor = effects
        && reachable.functions.iter().any(|f| {
            f.name == "ripple_browser_executor"
                || rcc_backend::optimize::function_symbols(f).contains("ripple_browser_executor")
        });
    if executor {
        roots.extend(browser::EXECUTOR_ROOTS);
    }
    let known: BTreeSet<_> = reachable
        .functions
        .iter()
        .map(|f| f.name.as_str())
        .chain(reachable.globals.iter().map(|g| g.name.as_str()))
        .collect();
    let needs_runtime = reachable.functions.iter().any(|f| {
        f.is_external
            || rcc_backend::optimize::function_symbols(f)
                .iter()
                .any(|n| !known.contains(n.as_str()))
    });
    let needs_runtime = needs_runtime
        || reachable.globals.iter().any(|g| {
            let mut symbols = BTreeSet::new();
            if let Some(value) = &g.initializer {
                rcc_backend::optimize::value_symbols(value, &mut symbols);
            }
            symbols.iter().any(|n| !known.contains(n.as_str()))
        });
    for runtime in if needs_runtime {
        runtime::modules(browser)?
    } else {
        &[]
    } {
        let mut runtime = runtime.clone();
        runtime
            .functions
            .retain(|f| !definitions.contains(f.name.as_str()));
        units.push(runtime);
    }
    // Compiler-supplied heap bounds are ordinary relocated globals, visible
    // solely to the private allocator. Fill their values after reachability.
    let mut bounds = rcc_ir::ir::Module::new("wasm-bounds".into());
    bounds.target = Target::Wasm;
    for (name, ty) in [
        ("__wasm_heap_base", IrType::FatPtr(Box::new(IrType::I16))),
        ("__wasm_heap_cells", IrType::I32),
    ] {
        bounds.add_global(GlobalVariable {
            name: name.into(),
            var_type: ty,
            is_constant: true,
            initializer: None,
            linkage: rcc_ir::ir::Linkage::External,
            symbol_id: None,
        });
    }
    for (name, params, result) in [
        ("__wasm_trap", vec![IrType::I16], IrType::Void),
        ("__wasm_exit", vec![IrType::I16], IrType::Void),
        (
            "__wasm_host_write",
            vec![IrType::FatPtr(Box::new(IrType::I16)), IrType::I32],
            IrType::Void,
        ),
        (
            "__wasm_dom_commit",
            vec![IrType::FatPtr(Box::new(IrType::I16)), IrType::I32],
            IrType::Void,
        ),
        (
            "__wasm_effect_commit",
            vec![IrType::FatPtr(Box::new(IrType::I16)), IrType::I32],
            IrType::Void,
        ),
        ("__rcc_f64_floor", vec![IrType::I64], IrType::I64),
        ("__rcc_f64_sqrt", vec![IrType::I64], IrType::I64),
        ("__rcc_f64_from_u64", vec![IrType::I64], IrType::I64),
    ] {
        if !definitions.contains(name) {
            let mut function = rcc_ir::ir::Function::new(name.into(), result);
            function.is_external = true;
            function.parameters = params
                .into_iter()
                .enumerate()
                .map(|(i, ty)| (i as u32, ty))
                .collect();
            bounds.add_function(function);
        }
    }
    units.push(bounds);
    let mut module = rcc_ir::link::link_modules(&units, Target::Wasm, &roots)?;
    rcc_ir::link::hoist_static_allocas(&mut module);
    let layout = globals::Layout::new(&module)?;
    let heap = ((layout.heap + 7) / 8) * 8 + 1;
    for global in &mut module.globals {
        if global.name == "__wasm_heap_base" {
            global.initializer = Some(Value::Constant(globals::pointer(heap) as i64));
        }
        if global.name == "__wasm_heap_cells" {
            global.initializer = Some(Value::Constant((memory::CELLS - heap) as i64));
        }
    }
    let mut types = TypeSection::new();
    let mut signatures = BTreeMap::new();
    let mut signature = |arity: usize| {
        let next = signatures.len() as u32;
        *signatures.entry(arity).or_insert_with(|| {
            types
                .ty()
                .function(vec![ValType::I64; arity], [ValType::I64]);
            next
        })
    };
    // Helpers use the same internal carriers, with separate checked addresses.
    let address_type = signature(2);
    let gep_type = signature(2);
    for f in &module.functions {
        if f.is_vararg {
            return Err(format!(
                "Wasm variadic function {} is not implemented yet",
                f.name
            ));
        }
        if f.return_type != IrType::Void {
            emit::width(&f.return_type)?;
        }
        for (_, ty) in &f.parameters {
            emit::width(ty)?;
        }
        signature(f.parameters.len());
        for inst in f.blocks.iter().flat_map(|b| &b.instructions) {
            if let rcc_ir::ir::Instruction::Call { args, .. } = inst {
                signature(args.len());
            }
        }
    }
    let write = module.get_function("__wasm_host_write").is_some();
    let dom = module.get_function("__wasm_dom_commit").is_some();
    let effect_import = module.get_function("__wasm_effect_commit").is_some();
    let host_type = signatures.len() as u32;
    if write || dom || effect_import {
        types.ty().function([ValType::I32, ValType::I32], []);
    }
    let import_count = u32::from(write) + u32::from(dom) + u32::from(effect_import);
    let public_types = browser.then(|| browser::types(&mut types));
    let address = import_count;
    let gep = address + 1;
    let mut functions = FunctionSection::new();
    functions.function(address_type);
    functions.function(gep_type);
    let mut indices = BTreeMap::new();
    let mut codes = BTreeMap::new();
    for (i, f) in module.functions.iter().enumerate() {
        functions.function(signatures[&f.parameters.len()]);
        indices.insert(f.name.clone(), (import_count + 2 + i as u32, f));
        codes.insert(f.name.clone(), 0x8000_0000 | (i as u64 + 1));
        if f.is_external {
            services::validate(f)?;
        }
    }
    let mut table = TableSection::new();
    table.table(TableType {
        element_type: RefType::FUNCREF,
        table64: false,
        minimum: module.functions.len() as u64 + 1,
        maximum: Some(module.functions.len() as u64 + 1),
        shared: false,
    });
    let mut elements = ElementSection::new();
    let entries: Vec<_> = module
        .functions
        .iter()
        .map(|f| indices[&f.name].0)
        .collect();
    elements.active(
        Some(0),
        &ConstExpr::i32_const(1),
        Elements::Functions(Cow::Borrowed(&entries)),
    );
    let mut memories = MemorySection::new();
    let pages = (memory::CELLS * 2).div_ceil(65536);
    memories.memory(MemoryType {
        minimum: pages,
        maximum: Some(pages),
        memory64: false,
        shared: false,
        page_size_log2: None,
    });
    let mut globals = GlobalSection::new();
    globals.global(
        GlobalType {
            val_type: ValType::I64,
            mutable: true,
            shared: false,
        },
        &ConstExpr::i64_const((2 * memory::BANK + 1) as i64),
    );
    for _ in 0..2 {
        globals.global(
            GlobalType {
                val_type: ValType::I32,
                mutable: true,
                shared: false,
            },
            &ConstExpr::i32_const(0),
        );
    }
    let mut exports = ExportSection::new();
    for name in &options.exports {
        exports.export(name, ExportKind::Func, indices[name].0);
    }
    exports.export("memory", ExportKind::Memory, 0);
    exports.export("__ripple_fault", ExportKind::Global, memory::FAULT);
    exports.export("__ripple_exit_status", ExportKind::Global, memory::EXIT);
    if let Some((index, _)) = indices.get("__wasm_flush") {
        exports.export("__ripple_flush", ExportKind::Func, *index);
    }
    let mut code = CodeSection::new();
    code.function(&memory::address());
    code.function(&memory::gep(address));
    let mut statistics = Statistics::default();
    let context = emit::Context {
        functions: &indices,
        codes: &codes,
        layout: &layout,
        address,
        gep,
        signatures: &signatures,
    };
    for f in &module.functions {
        if f.is_external {
            code.function(&services::emit(
                f,
                address,
                u32::from(write),
                u32::from(write) + u32::from(dom),
            )?);
        } else {
            let (body, structured) = emit::emit(f, &context)?;
            code.function(&body);
            statistics.functions += 1;
            if structured {
                statistics.structured_functions += 1;
            } else {
                statistics.dispatch_functions += 1;
            }
        }
    }
    if let Some(public_types) = public_types {
        browser::emit(
            &indices,
            address,
            effects,
            executor,
            &public_types,
            &mut functions,
            &mut exports,
            &mut code,
            import_count + 2 + module.functions.len() as u32,
        )?;
    }
    let mut wasm = wasm_encoder::Module::new();
    wasm.section(&types);
    if write || dom || effect_import {
        let mut imports = ImportSection::new();
        if write {
            imports.import("ripple", "write", EntityType::Function(host_type));
        }
        if dom {
            imports.import("ripple", "dom_commit", EntityType::Function(host_type));
        }
        if effect_import {
            imports.import("ripple", "effect_commit", EntityType::Function(host_type));
        }
        wasm.section(&imports);
    }
    wasm.section(&functions)
        .section(&table)
        .section(&memories)
        .section(&globals)
        .section(&exports)
        .section(&elements)
        .section(&code)
        .section(&layout.data(&module, &codes)?);
    let bytes = wasm.finish();
    wasmparser::Validator::new()
        .validate_all(&bytes)
        .map_err(|e| format!("generated invalid Wasm: {e}"))?;
    Ok(Artifact { bytes, statistics })
}
