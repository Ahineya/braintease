//! RFL compiler. Source in, Ripple IR out. Calls use heap activation records
//! so RFL recursion does not grow the hardware stack. See `lowering.md`.

mod ast;
mod auto_import;
mod check;
mod completion;
mod error;
mod ide;
mod lexer;
mod lower;
mod lsp;
mod modules;
mod native;
mod parser;
mod run;
mod ty;
mod typed;

pub use error::CompileError;
pub use native::{compile_file_to_native, run_native_file, run_native_program};
pub use rcc_ir::target::Target;
pub use lsp::serve as serve_lsp;
pub use rcc_ir::ir::Module;
pub use run::{run_file, run_file_in_terminal, run_program, RunResult};

/// Check a standalone source without constructing executable IR. Libraries
/// may omit `main`; imports require [`check_file`] so their paths can resolve.
pub fn check_source(source: &str) -> Result<(), CompileError> {
    let tokens = lexer::tokenize(source)?;
    let ast = parser::parse(source, &tokens)?;
    if !ast.imports.is_empty() {
        return Err(CompileError::plain("import needs a source file"));
    }
    check::check(source, &ast).map(|_| ())
}

/// Typecheck a file and its imports without lowering RFL functions.
pub fn check_file(path: &std::path::Path) -> Result<(), CompileError> {
    modules::check_path(path).map(|_| ())
}

pub fn compile_to_ir(source: &str) -> Result<Module, CompileError> {
    compile_to_ir_for_target(source, Target::Ripple)
}

pub fn compile_to_ir_for_target(source: &str, target: Target) -> Result<Module, CompileError> {
    let tokens = lexer::tokenize(source)?;
    let ast = parser::parse_for_target(source, &tokens, target)?;
    if !ast.imports.is_empty() {
        return Err(CompileError::plain("import needs a source file"));
    }
    let typed = check::check(source, &ast)?;
    let mut module = lower::lower("program", &typed)?;
    module.target = target;
    Ok(module)
}

pub fn compile_file(path: &std::path::Path) -> Result<Module, CompileError> {
    modules::compile_path(path)
}

pub fn compile_file_for_target(path: &std::path::Path, target: Target) -> Result<Module, CompileError> {
    modules::compile_path_for_target(path, target)
}

pub fn check_file_for_target(path: &std::path::Path, target: Target) -> Result<(), CompileError> {
    modules::check_path_for_target(path, target).map(|_| ())
}

pub fn compile_to_ir_text(source: &str) -> Result<String, CompileError> {
    let module = compile_to_ir(source)?;
    Ok(rcc_ir::emit_module(&module))
}

pub fn compile_file_to_ir_text(path: &std::path::Path) -> Result<String, CompileError> {
    let module = compile_file(path)?;
    Ok(rcc_ir::emit_module(&module))
}

pub fn compile_to_asm(source: &str) -> Result<String, CompileError> {
    asm_of(&compile_to_ir(source)?)
}

pub fn compile_file_to_asm(path: &std::path::Path) -> Result<String, CompileError> {
    asm_of(&compile_file(path)?)
}

fn asm_of(module: &Module) -> Result<String, CompileError> {
    let has_main = module.functions.iter().any(|func| func.name == "main");
    let instructions = rcc_backend::lower_module_to_assembly_with_options(
        module,
        rcc_backend::LoweringOptions {
            bank_size: 64000,
            use_v2: true,
            trace_spills: false,
            segmented_stack: false,
        },
    )
    .map_err(CompileError::plain)?;
    let mut asm = rcc_codegen::emit::emit_complete_program(instructions, has_main)
        .map_err(|err| CompileError::plain(err.to_string()))?;
    asm.push_str(RFL_FAIL_ASM);
    Ok(asm)
}

/// `rfl_fail` prints the reason and calls this. The backend has no halt instruction.
const RFL_FAIL_ASM: &str = "
rfl_halt:
    LI RV0, 1
    HALT
";
