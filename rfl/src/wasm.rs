//! WebAssembly compilation and Node execution. Browser embedding uses the same
//! module; the CLI harness supplies only the documented buffered-output import.
use crate::{CompileError, RunResult, Target};
use std::{
    path::{Path, PathBuf},
    process::Command,
};
pub fn compile_file_to_wasm(input: &Path, output: &Path) -> Result<(), CompileError> {
    write(
        &crate::compile_file_for_target(input, Target::Wasm)?,
        output,
    )
}
fn write(module: &crate::Module, output: &Path) -> Result<(), CompileError> {
    let artifact = rcc_wasm::compile(std::slice::from_ref(module), &rcc_wasm::Options::default())
        .map_err(CompileError::plain)?;
    std::fs::write(output, artifact.bytes)
        .map_err(|e| CompileError::plain(format!("cannot write {}: {e}", output.display())))
}
pub fn run_wasm_file(input: &Path, terminal: bool) -> Result<RunResult, CompileError> {
    execute(
        &crate::compile_file_for_target(input, Target::Wasm)?,
        terminal,
    )
}
pub fn run_wasm_program(source: &str) -> Result<RunResult, CompileError> {
    execute(
        &crate::compile_to_ir_for_target(source, Target::Wasm)?,
        false,
    )
}
fn execute(module: &crate::Module, terminal: bool) -> Result<RunResult, CompileError> {
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|e| CompileError::plain(e.to_string()))?
        .as_nanos();
    let path = std::env::temp_dir().join(format!("rfl-wasm-{}-{stamp}.wasm", std::process::id()));
    let _cleanup = Artifact(path.clone());
    write(module, &path)?;
    let mut command = Command::new(std::env::var_os("NODE").unwrap_or_else(|| "node".into()));
    command
        .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../rcc-wasm/runner.mjs"))
        .arg(&path);
    if terminal {
        let status = command
            .status()
            .map_err(|e| CompileError::plain(format!("cannot run Wasm (Node is required): {e}")))?;
        Ok(RunResult {
            status: status.code().unwrap_or(1),
            stdout: String::new(),
        })
    } else {
        let output = command
            .output()
            .map_err(|e| CompileError::plain(format!("cannot run Wasm (Node is required): {e}")))?;
        if !output.status.success() && !output.stderr.is_empty() {
            return Err(CompileError::plain(
                String::from_utf8_lossy(&output.stderr).into_owned(),
            ));
        }
        Ok(RunResult {
            status: output.status.code().unwrap_or(1),
            stdout: String::from_utf8_lossy(&output.stdout).into_owned(),
        })
    }
}
struct Artifact(PathBuf);
impl Drop for Artifact {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}
