//! Native C test execution uses argument arrays and drains both pipes while
//! waiting, so diagnostics cannot deadlock a compiler process.
use crate::{
    compiler::{CompilationResult, ToolPaths},
    config::RunConfig,
};
use anyhow::{Context, Result};
use std::{
    io::Read,
    path::Path,
    process::{Command, Stdio},
    time::{Duration, Instant},
};
fn run(mut command: Command, seconds: u64) -> Result<(bool, String, String)> {
    let mut child = command
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .context("native test command failed to start")?;
    let mut stdout = child.stdout.take().unwrap();
    let mut stderr = child.stderr.take().unwrap();
    let out = std::thread::spawn(move || {
        let mut b = String::new();
        stdout.read_to_string(&mut b).map(|_| b)
    });
    let err = std::thread::spawn(move || {
        let mut b = String::new();
        stderr.read_to_string(&mut b).map(|_| b)
    });
    let start = Instant::now();
    let mut timed_out = false;
    let status = loop {
        if let Some(s) = child.try_wait()? {
            break s;
        }
        if start.elapsed() >= Duration::from_secs(seconds) {
            timed_out = true;
            child.kill()?;
            break child.wait()?;
        }
        std::thread::sleep(Duration::from_millis(20));
    };
    let stdout = out
        .join()
        .map_err(|_| anyhow::anyhow!("native stdout reader panicked"))??;
    let mut stderr = err
        .join()
        .map_err(|_| anyhow::anyhow!("native stderr reader panicked"))??;
    if timed_out {
        stderr = format!("Timeout after {seconds}s: {stderr}");
    }
    Ok((status.success() && !timed_out, stdout, stderr))
}
pub(crate) fn compile_and_run(
    input: &Path,
    tools: &ToolPaths,
    config: &RunConfig,
) -> Result<CompilationResult> {
    let name = input.file_stem().context("C test has no name")?;
    let exe = tools.build_dir.join(name).with_extension("native");
    let ir = exe.with_extension("native.ir");
    let mut compile = Command::new(&tools.rcc);
    compile
        .arg("compile")
        .arg(input)
        .args(["--target", "native", "-I"])
        .arg(tools.runtime_dir.join("include"))
        .arg("-o")
        .arg(&exe)
        .args(["--save-ir", "--ir-output"])
        .arg(ir);
    let (ok, _, error) = run(compile, config.timeout_secs.max(10))?;
    if !ok {
        return Ok(CompilationResult {
            success: false,
            output: String::new(),
            has_provenance_warning: false,
            error_message: Some(format!("Native compilation failed: {error}")),
        });
    }
    let (ok, output, error) = run(Command::new(exe), config.timeout_secs)?;
    Ok(CompilationResult {
        success: ok,
        output,
        has_provenance_warning: false,
        error_message: (!ok).then_some(format!("Native execution failed: {error}")),
    })
}
