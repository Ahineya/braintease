//! Native object linking and execution, separate from the Ripple tool pipeline.
use crate::{CompileError, RunResult, Target};
use std::path::{Path, PathBuf};
use std::process::Command;

pub fn compile_file_to_native(input: &Path, output: &Path) -> Result<(), CompileError> {
    let module = crate::compile_file_for_target(input, Target::Native)?;
    rcc_native::compile_executable(&module, output).map_err(CompileError::plain)
}
pub fn run_native_file(input: &Path, terminal: bool) -> Result<RunResult, CompileError> {
    let module = crate::compile_file_for_target(input, Target::Native)?;
    execute(&module, terminal)
}
pub fn run_native_program(source: &str) -> Result<RunResult, CompileError> {
    let module = crate::compile_to_ir_for_target(source, Target::Native)?;
    execute(&module, false)
}
fn execute(module: &crate::Module, terminal: bool) -> Result<RunResult, CompileError> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../target/rfl-native");
    std::fs::create_dir_all(&root).map_err(|e| CompileError::plain(e.to_string()))?;
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|e| CompileError::plain(e.to_string()))?
        .as_nanos();
    let executable = root.join(format!("program-{}-{stamp}", std::process::id()));
    let _cleanup = Executable(executable.clone());
    rcc_native::compile_executable(module, &executable).map_err(CompileError::plain)?;
    let mut command = Command::new(&executable);
    if terminal {
        let status = command
            .status()
            .map_err(|e| CompileError::plain(format!("cannot run native program: {e}")))?;
        Ok(RunResult {
            status: status.code().unwrap_or(1),
            stdout: String::new(),
        })
    } else {
        let result = command
            .output()
            .map_err(|e| CompileError::plain(format!("cannot run native program: {e}")))?;
        Ok(RunResult {
            status: result.status.code().unwrap_or(1),
            stdout: String::from_utf8_lossy(&result.stdout).into_owned(),
        })
    }
}
struct Executable(PathBuf);
impl Drop for Executable {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}
