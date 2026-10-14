//! RFL compiler. Source in, Ripple IR out. Calls use heap activation records
//! so RFL recursion does not grow the hardware stack. See `lowering.md`.

mod ast;
mod check;
mod error;
mod lexer;
mod lower;
mod parser;
mod run;
mod ty;
mod typed;

pub use error::CompileError;
pub use rcc_ir::ir::Module;
pub use run::{run_program, RunResult};

pub fn compile_to_ir(source: &str) -> Result<Module, CompileError> {
    let tokens = lexer::tokenize(source)?;
    let ast = parser::parse(source, &tokens)?;
    let typed = check::check(source, &ast)?;
    lower::lower("program", &typed)
}

pub fn compile_to_ir_text(source: &str) -> Result<String, CompileError> {
    let module = compile_to_ir(source)?;
    Ok(rcc_ir::emit_module(&module))
}

pub fn compile_to_asm(source: &str) -> Result<String, CompileError> {
    let module = compile_to_ir(source)?;
    let has_main = module.functions.iter().any(|func| func.name == "main");
    let instructions = rcc_backend::lower_module_to_assembly_with_options(
        &module,
        rcc_backend::LoweringOptions {
            bank_size: 64000,
            use_v2: true,
            trace_spills: false,
        },
    )
    .map_err(CompileError::plain)?;
    let mut asm = rcc_codegen::emit::emit_complete_program(instructions, has_main)
        .map_err(|err| CompileError::plain(err.to_string()))?;
    asm.push_str(RFL_FAIL_ASM);
    Ok(asm)
}

/// Prints `fail` and halts. The backend leaves `rfl_fail` unresolved.
const RFL_FAIL_ASM: &str = "
rfl_fail:
    LI A0, 102
    STORE A0, R0, R0
    LI A0, 97
    STORE A0, R0, R0
    LI A0, 105
    STORE A0, R0, R0
    LI A0, 108
    STORE A0, R0, R0
    LI A0, 10
    STORE A0, R0, R0
    LI RV0, 1
    HALT
";
