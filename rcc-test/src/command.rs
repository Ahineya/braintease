use anyhow::{Context, Result};
use std::path::Path;
use std::process::Stdio;
use std::time::Duration;
use tokio::process::Command;
use tokio::time::timeout;

/// Result of running a command
#[derive(Debug)]
pub struct CommandResult {
    pub exit_code: i32,
    pub stdout: String,
    pub stderr: String,
    pub timed_out: bool,
}

/// Run a shell command with timeout
pub async fn run_command(cmd: &str, timeout_secs: u64) -> Result<CommandResult> {
    let child = Command::new("sh")
        .env("RIPPLE_NET", "off")
        .env("RCC_NATIVE_NET", "off")
        .arg("-c")
        .arg(cmd)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .context(format!("Failed to spawn command: {cmd}"))?;

    let timeout_duration = Duration::from_secs(timeout_secs);

    // Try to wait for the process with timeout
    let result = timeout(timeout_duration, child.wait_with_output()).await;

    match result {
        Ok(Ok(output)) => Ok(CommandResult {
            exit_code: output.status.code().unwrap_or(-1),
            stdout: String::from_utf8_lossy(&output.stdout).to_string(),
            stderr: String::from_utf8_lossy(&output.stderr).to_string(),
            timed_out: false,
        }),
        Ok(Err(e)) => Err(e.into()),
        Err(_) => {
            // Timeout occurred
            Ok(CommandResult {
                exit_code: -1,
                stdout: String::new(),
                stderr: format!("Timeout after {timeout_secs}s"),
                timed_out: true,
            })
        }
    }
}

/// Run a command synchronously (wrapper for async version)
pub fn run_command_sync(cmd: &str, timeout_secs: u64) -> Result<CommandResult> {
    // Use std::process instead of tokio to avoid runtime conflicts in parallel execution
    use std::io::Read;
    use std::process::{Command, Stdio};
    use std::time::Instant;

    let start = Instant::now();
    let mut child = Command::new("sh")
        .env("RIPPLE_NET", "off")
        .env("RCC_NATIVE_NET", "off")
        .arg("-c")
        .arg(cmd)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .context(format!("Failed to spawn command: {cmd}"))?;

    // Drain both pipes concurrently. Waiting for exit first deadlocks when a
    // verbose compiler fills either OS pipe buffer.
    fn drain(
        mut stream: impl Read + Send + 'static,
    ) -> std::thread::JoinHandle<std::io::Result<Vec<u8>>> {
        std::thread::spawn(move || {
            let mut bytes = Vec::new();
            stream.read_to_end(&mut bytes)?;
            Ok(bytes)
        })
    }
    let stdout = drain(child.stdout.take().context("missing child stdout")?);
    let stderr = drain(child.stderr.take().context("missing child stderr")?);

    // Poll the child process with a timeout
    let timeout_duration = Duration::from_secs(timeout_secs);
    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                let stdout = stdout
                    .join()
                    .map_err(|_| anyhow::anyhow!("stdout reader panicked"))??;
                let stderr = stderr
                    .join()
                    .map_err(|_| anyhow::anyhow!("stderr reader panicked"))??;
                return Ok(CommandResult {
                    exit_code: status.code().unwrap_or(-1),
                    stdout: String::from_utf8_lossy(&stdout).into_owned(),
                    stderr: String::from_utf8_lossy(&stderr).into_owned(),
                    timed_out: false,
                });
            }
            Ok(None) => {
                // Still running
                if start.elapsed() >= timeout_duration {
                    // Timeout - kill the process
                    let _ = child.kill();
                    let _ = child.wait(); // Reap the zombie

                    return Ok(CommandResult {
                        exit_code: -1,
                        stdout: String::new(),
                        stderr: format!("Timeout after {timeout_secs}s"),
                        timed_out: true,
                    });
                }
                // Sleep briefly before checking again
                std::thread::sleep(Duration::from_millis(100));
            }
            Err(e) => {
                return Err(anyhow::anyhow!("Failed to wait for process: {}", e));
            }
        }
    }
}

/// Check if a command/binary exists
pub fn command_exists(cmd: &str) -> bool {
    std::process::Command::new("which")
        .arg(cmd)
        .output()
        .map(|output| output.status.success())
        .unwrap_or(false)
}

/// Clean up a file if it exists
pub fn cleanup_file(path: &Path) -> Result<()> {
    if path.exists() {
        std::fs::remove_file(path).context(format!("Failed to remove file: {}", path.display()))?;
    }
    Ok(())
}

/// Clean up multiple files matching a pattern
pub fn cleanup_pattern(pattern: &str) -> Result<usize> {
    let mut count = 0;
    for path in (glob::glob(pattern)?).flatten() {
        cleanup_file(&path)?;
        count += 1;
    }
    Ok(count)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn synchronous_commands_drain_both_pipes_while_running() {
        let result = run_command_sync(
            "exec python3 -c 'import sys; sys.stdout.write(\"x\" * 200000); sys.stderr.write(\"y\" * 200000)'", 2
        ).unwrap();
        assert!(
            !result.timed_out,
            "a full output pipe must not block the child"
        );
        assert_eq!(result.exit_code, 0);
        assert_eq!(result.stdout.len(), 200000);
        assert_eq!(result.stderr.len(), 200000);
    }
}
