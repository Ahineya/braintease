//! Host process entry, logical globals and function-token initialization.
use super::*;
use cranelift_codegen::ir::InstBuilder;
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext};
pub(super) fn emit_entry(compiler: &mut Compiler, kernel_cells: u64) -> Result<(), String> {
    let main = compiler
        .ir
        .get_function("main")
        .ok_or("native executable needs main")?;
    let main_arguments = main.parameters.len();
    if main.is_external || !matches!(main_arguments, 0 | 2) {
        return Err("native main must have zero parameters or (argc, argv)".into());
    }
    let initializers = super::globals::initializers(compiler)?;
    let mut context = compiler.object.make_context();
    context.func.signature = compiler.signature(0);
    let id = compiler
        .object
        .declare_function("rcc_native_entry", Linkage::Local, &context.func.signature)
        .map_err(|e| e.to_string())?;
    let mut frontend = FunctionBuilderContext::new();
    {
        let mut b = FunctionBuilder::new(&mut context.func, &mut frontend);
        let entry = b.create_block();
        b.append_block_params_for_function_params(entry);
        b.switch_to_block(entry);
        let runtime = b.block_params(entry)[0];
        if compiler.resumable {
            let id = compiler.import("rcc_native_process_memory", 0)?;
            let memory = compiler.object.declare_func_in_func(id, b.func);
            b.ins().call(memory, &[runtime]);
        }
        if kernel_cells != 0 {
            let id = compiler.import("rcc_native_kernel_globals", 1)?;
            let reserve = compiler.object.declare_func_in_func(id, b.func);
            let cells = b.ins().iconst(types::I64, kernel_cells as i64);
            b.ins().call(reserve, &[runtime, cells]);
        }
        let store = compiler
            .object
            .declare_func_in_func(compiler.imports["rcc_native_store"], b.func);
        for (pointer, value, words) in initializers {
            let p = b.ins().iconst(types::I64, pointer as i64);
            let v = b.ins().iconst(types::I64, value as i64);
            let w = b.ins().iconst(types::I64, words as i64);
            b.ins().call(store, &[runtime, p, v, w]);
        }
        let register = compiler
            .object
            .declare_func_in_func(compiler.imports["rcc_native_register"], b.func);
        let functions: BTreeMap<String, usize> = compiler
            .ir
            .functions
            .iter()
            .filter(|f| !f.is_external)
            .map(|f| {
                (
                    f.name.clone(),
                    f.parameters.len() + usize::from(f.is_vararg),
                )
            })
            .chain(compiler.address_taken.iter().map(|name| {
                let arity = compiler
                    .ir
                    .get_function(name)
                    .map(|f| f.parameters.len() + usize::from(f.is_vararg))
                    .or_else(|| {
                        rcc_ir::target::NATIVE_SERVICES
                            .iter()
                            .find(|(symbol, _)| *symbol == name)
                            .map(|(_, arity)| *arity)
                    })
                    .unwrap();
                (name.clone(), arity)
            }))
            .collect();
        for (name, arity) in functions {
            let reference = compiler
                .object
                .declare_func_in_func(compiler.functions[&name], b.func);
            let address = b.ins().func_addr(types::I64, reference);
            let code = b.ins().iconst(types::I64, compiler.codes[&name] as i64);
            let count = b.ins().iconst(types::I64, arity as i64);
            // The arity is packed above the 32-bit function token.
            let count = b.ins().ishl_imm_u(count, 32);
            let token = b.ins().bor(code, count);
            b.ins().call(register, &[runtime, token, address]);
        }
        if let Some(&errno) = compiler.globals.get("errno") {
            let id = compiler.import("rcc_native_errno", 1)?;
            let reference = compiler.object.declare_func_in_func(id, b.func);
            let address = b.ins().iconst(types::I64, errno as i64);
            b.ins().call(reference, &[runtime, address]);
        }
        let mut arguments = vec![runtime];
        if main_arguments == 2 {
            let id = compiler.import("rcc_native_arguments", 0)?;
            let reference = compiler.object.declare_func_in_func(id, b.func);
            let call = b.ins().call(reference, &[runtime]);
            let packed = b.inst_results(call)[0];
            let argc = b.ins().ushr_imm_u(packed, 32);
            let argv = b.ins().band_imm_u(packed, 0xffff_ffff);
            arguments.extend([argc, argv]);
        }
        let call = if compiler.resumable {
            let id = compiler.import("rcc_native_process_start", 4)?;
            let start = compiler.object.declare_func_in_func(id, b.func);
            let token = b.ins().iconst(types::I64, compiler.codes["main"] as i64);
            let zero = b.ins().iconst(types::I64, 0);
            let hosted = b
                .ins()
                .iconst(types::I64, i64::from(super::process::hosted(&compiler.ir)));
            b.ins().call(
                start,
                &[
                    runtime,
                    token,
                    arguments.get(1).copied().unwrap_or(zero),
                    arguments.get(2).copied().unwrap_or(zero),
                    hosted,
                ],
            )
        } else {
            let main = compiler
                .object
                .declare_func_in_func(compiler.functions["main"], b.func);
            b.ins().call(main, &arguments)
        };
        let result = b.inst_results(call)[0];
        b.ins().return_(&[result]);
        b.seal_all_blocks();
        b.finalize(compiler.object.target_config());
    }
    compiler
        .object
        .define_function(id, &mut context)
        .map_err(|e| e.to_string())?;
    let mut context = compiler.object.make_context();
    context
        .func
        .signature
        .returns
        .push(AbiParam::new(types::I32));
    let main_id = compiler
        .object
        .declare_function("main", Linkage::Export, &context.func.signature)
        .map_err(|e| e.to_string())?;
    let mut start_sig = compiler.object.make_signature();
    start_sig.params.push(AbiParam::new(types::I64));
    start_sig.returns.push(AbiParam::new(types::I32));
    let start = compiler
        .object
        .declare_function("rcc_native_start", Linkage::Import, &start_sig)
        .map_err(|e| e.to_string())?;
    let mut frontend = FunctionBuilderContext::new();
    {
        let mut b = FunctionBuilder::new(&mut context.func, &mut frontend);
        let entry = b.create_block();
        b.switch_to_block(entry);
        let program = compiler.object.declare_func_in_func(id, b.func);
        let address = b.ins().func_addr(types::I64, program);
        let start = compiler.object.declare_func_in_func(start, b.func);
        let call = b.ins().call(start, &[address]);
        let result = b.inst_results(call)[0];
        b.ins().return_(&[result]);
        b.seal_all_blocks();
        b.finalize(compiler.object.target_config());
    }
    compiler
        .object
        .define_function(main_id, &mut context)
        .map_err(|e| e.to_string())?;
    Ok(())
}
