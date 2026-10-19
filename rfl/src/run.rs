//! Assemble, link, and run an RFL program on rvm or the Brainfuck CPU.
//!
//! The guest status is the full `i16` in `RV0`. This reports the low 8 bits,
//! which is what a host process can return.

use std::path::PathBuf;
use std::process::Command;

use crate::error::CompileError;

pub struct RunResult {
    pub status: i32,
    pub stdout: String,
}

pub fn run_program(source: &str) -> Result<RunResult, CompileError> {
    let asm = crate::compile_to_asm(source)?;
    run_asm(&asm, false, false, Backend::Rvm)
}

pub fn run_file(path: &std::path::Path) -> Result<RunResult, CompileError> {
    let asm = crate::compile_file_to_asm(path)?;
    run_asm(&asm, false, false, Backend::Rvm)
}

/// Run with the host terminal attached. `getchar` can read keystrokes, and
/// the guest's own output is shown as it is produced. There is no wall-time
/// limit, because the program may be waiting for a person.
pub fn run_file_in_terminal(path: &std::path::Path, jit: bool) -> Result<i32, CompileError> {
    eprintln!("   Compiling {}", path.display());
    let asm = crate::compile_file_to_asm(path)?;
    eprintln!("    Finished");
    eprintln!("     Running `{}`", path.display());
    let result = run_asm(&asm, true, jit, Backend::Rvm)?;
    Ok(result.status)
}

/// Compile the original Brainfuck CPU image and run it with inherited guest
/// stdio. Lightweight telemetry stays on stderr; IRQ polling remains disabled.
pub fn run_file_in_brainfuck(path: &std::path::Path, jit: bool) -> Result<i32, CompileError> {
    eprintln!("   Compiling {}", path.display());
    let asm = crate::compile_file_to_asm(path)?;
    eprintln!("   Exporting Brainfuck CPU image");
    run_asm(&asm, true, jit, Backend::Rbf).map(|result| result.status)
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Backend {
    Rvm,
    Rbf,
}

fn run_asm(
    asm: &str,
    terminal: bool,
    jit: bool,
    backend: Backend,
) -> Result<RunResult, CompileError> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..");
    let dir = root.join("target/rfl-run");
    std::fs::create_dir_all(&dir)
        .map_err(|err| CompileError::plain(format!("cannot create {}: {err}", dir.display())))?;
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let stem = dir.join(format!("prog-{stamp}-{}", std::process::id()));
    let _cleanup = RunArtifacts(stem.clone());
    let asm_path = stem.with_extension("asm");
    let pobj_path = stem.with_extension("pobj");
    let bin_path = stem.with_extension("bin");
    let bf_path = stem.with_extension("bf");
    std::fs::write(&asm_path, asm).map_err(|err| {
        CompileError::plain(format!("cannot write {}: {err}", asm_path.display()))
    })?;

    let rasm = root.join("src/ripple-asm/target/release/rasm");
    let rlink = root.join("src/ripple-asm/target/release/rlink");
    let crt0 = root.join("runtime/crt0.pobj");
    let mut assemble = Command::new(&rasm);
    assemble
        .arg("assemble")
        .arg(&asm_path)
        .args(["-o"])
        .arg(&pobj_path)
        .args(["--bank-size", "64000", "--max-immediate", "65535"]);
    tool(&mut assemble)?;
    let mut link = Command::new(&rlink);
    link.arg(&crt0).arg(&pobj_path);
    if asm.contains("rfl_frame_alloc")
        || asm.contains("putchar")
        || asm.contains("puts")
        || asm.contains("printf")
        || asm.contains("getchar")
        || asm.contains("mmio_read")
        || asm.contains("mmio_write")
        || asm.contains("malloc")
    {
        link.arg(root.join("runtime/libruntime.par"));
    }
    match backend {
        Backend::Rvm => {
            link.args(["-f", "binary"]);
        }
        Backend::Rbf => {
            link.args([
                "-f",
                "bf",
                "--standalone",
                "--bf-host",
                "--memory-banks",
                "16",
            ]);
        }
    }
    link.args(["--bank-size", "64000", "-o"])
        .arg(if backend == Backend::Rvm {
            &bin_path
        } else {
            &bf_path
        });
    tool(&mut link)?;

    let runner = match backend {
        Backend::Rvm => root.join("target/release/rvm"),
        Backend::Rbf => {
            let bytes = std::fs::metadata(&bf_path)
                .map_err(|err| {
                    CompileError::plain(format!("cannot inspect {}: {err}", bf_path.display()))
                })?
                .len();
            eprintln!("    Finished Brainfuck image: {bytes} bytes");
            eprintln!("     Running Brainfuck with lightweight telemetry");
            root.join("target/release/rbf")
        }
    };
    let mut command = Command::new(&runner);
    if backend == Backend::Rvm {
        command
            .arg(&bin_path)
            .args(["--exit-code", "--memory", "4294967296"]);
    } else {
        command
            .arg(&bf_path)
            .args(["--exit-code", "--telemetry-lite"]);
        if terminal {
            command.args(["--headless", "--tty-stdin"]);
        }
    }
    if jit {
        command.arg("--jit");
    }
    if terminal {
        let status = command.status().map_err(|err| {
            CompileError::plain(format!("cannot run {}: {err}", runner.display()))
        })?;
        return Ok(RunResult {
            status: status.code().unwrap_or(1),
            stdout: String::new(),
        });
    }
    // Tests capture output and must not take the keyboard. Headless disables
    // host input, so a guest that calls getchar would spin until this limit.
    let output = command
        .args(["--headless", "--timeout", "20"])
        .output()
        .map_err(|err| CompileError::plain(format!("cannot run {}: {err}", runner.display())))?;
    let stdout = String::from_utf8_lossy(&output.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&output.stderr);
    if stderr.contains("Linking failed")
        || stderr.contains("Runtime error")
        || stderr.contains("Failed to")
        || stderr.contains("Wall-time")
    {
        return Err(CompileError::plain(format!("rvm failed: {stderr}")));
    }
    let status = output.status.code().unwrap_or(1);
    Ok(RunResult { status, stdout })
}

fn tool(cmd: &mut Command) -> Result<(), CompileError> {
    let name = cmd.get_program().to_string_lossy().into_owned();
    let output = cmd
        .output()
        .map_err(|err| CompileError::plain(format!("cannot run tool {name}: {err}")))?;
    if output.status.success() {
        return Ok(());
    }
    let stderr = String::from_utf8_lossy(&output.stderr);
    let stdout = String::from_utf8_lossy(&output.stdout);
    Err(CompileError::plain(format!(
        "tool {name} failed:\n{stdout}{stderr}"
    )))
}

/// Remove only artifacts owned by this invocation, including failed tool runs.
struct RunArtifacts(PathBuf);

impl Drop for RunArtifacts {
    fn drop(&mut self) {
        for extension in ["asm", "pobj", "bin", "bf"] {
            let _ = std::fs::remove_file(self.0.with_extension(extension));
        }
    }
}
