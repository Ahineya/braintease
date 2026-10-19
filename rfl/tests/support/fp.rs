#![allow(dead_code)]
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};

pub fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("..")
}
pub struct Fixture(pub PathBuf);
impl Fixture {
    pub fn new(source: &str) -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let dir = loop {
            let dir = std::env::temp_dir().join(format!(
                "rfl-applicative-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
            ));
            match std::fs::create_dir(&dir) {
                Ok(()) => break dir,
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => panic!("cannot create {}: {e}", dir.display()),
            }
        };
        let file = dir.join("main.rfl");
        std::fs::write(&file, source).unwrap();
        Self(file)
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(self.0.parent().unwrap());
    }
}
pub fn parity(source: &str, expected: &str) {
    let f = Fixture::new(source);
    for native in [true, false] {
        let result = if native {
            rfl::run_native_file(&f.0, false)
        } else {
            rfl::run_file(&f.0)
        }
        .unwrap_or_else(|e| panic!("native={native}: {e}"));
        assert_eq!(result.status, 0, "native={native}: {}", result.stdout);
        assert_eq!(result.stdout, expected, "native={native}");
    }
}
pub fn tool(command: &mut Command) -> Output {
    let output = command.output().unwrap();
    assert!(
        output.status.success(),
        "{command:?}: {}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    output
}
pub fn object(f: &Fixture) -> PathBuf {
    let asm = f.0.with_extension("asm");
    let object = f.0.with_extension("pobj");
    std::fs::write(&asm, rfl::compile_file_to_asm(&f.0).unwrap()).unwrap();
    tool(
        Command::new(root().join("src/ripple-asm/target/release/rasm"))
            .arg("assemble")
            .arg(asm)
            .arg("-o")
            .arg(&object)
            .args(["--bank-size", "64000", "--max-immediate", "65535"]),
    );
    object
}
pub fn binary(f: &Fixture) -> PathBuf {
    let object = object(f);
    let binary = f.0.with_extension("bin");
    tool(
        Command::new(root().join("src/ripple-asm/target/release/rlink"))
            .arg(root().join("runtime/crt0.pobj"))
            .arg(object)
            .arg(root().join("runtime/libruntime.par"))
            .args(["-f", "binary", "--bank-size", "64000", "-o"])
            .arg(&binary),
    );
    binary
}
pub fn measured(source: &str, expected: &str, budget: u64) -> u64 {
    let f = Fixture::new(source);
    let binary = binary(&f);
    let report = f.0.with_extension("json");
    let output = tool(
        Command::new(root().join("target/release/rvm"))
            .arg(binary)
            .args([
                "--headless",
                "--exit-code",
                "--memory",
                "16384000",
                "--timeout",
                "20",
                "--report",
            ])
            .arg(&report)
            .arg("--max-instructions")
            .arg(budget.to_string()),
    );
    assert_eq!(String::from_utf8(output.stdout).unwrap(), expected);
    let report: serde_json::Value =
        serde_json::from_slice(&std::fs::read(report).unwrap()).unwrap();
    assert_eq!(report["success"], true);
    assert_eq!(report["stop_reason"], "halted");
    report["instructions"].as_u64().unwrap()
}
pub fn native_input(source: &str, input: &[u8], expected: &str) {
    use std::io::Write;
    let f = Fixture::new(source);
    let executable = f.0.with_extension("exe");
    rfl::compile_file_to_native(&f.0, &executable).unwrap();
    let mut child = Command::new(executable)
        .env("RCC_NATIVE_TIMEOUT", "15")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    child.stdin.take().unwrap().write_all(input).unwrap();
    let output = child.wait_with_output().unwrap();
    assert!(
        output.status.success(),
        "{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(String::from_utf8(output.stdout).unwrap(), expected);
}

pub fn native_pending_input(source: &str, expected: &str) {
    use std::time::{Duration, Instant};
    let f = Fixture::new(source);
    let executable = f.0.with_extension("exe");
    rfl::compile_file_to_native(&f.0, &executable).unwrap();
    let mut child = Command::new(executable)
        .env("RCC_NATIVE_TIMEOUT", "10")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    // Keep the pipe open and empty: a blocking getchar would hang this child.
    let input = child.stdin.take().unwrap();
    let deadline = Instant::now() + Duration::from_secs(15);
    loop {
        if child.try_wait().unwrap().is_some() {
            break;
        }
        if Instant::now() >= deadline {
            child.kill().unwrap();
            let _ = child.wait();
            panic!("console input blocked guest cancellation");
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    drop(input);
    let output = child.wait_with_output().unwrap();
    assert!(
        output.status.success(),
        "{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(String::from_utf8(output.stdout).unwrap(), expected);
}
pub fn brainfuck(source: &str, expected: &str) {
    let f = Fixture::new(source);
    let object = object(&f);
    let bf = f.0.with_extension("bf");
    tool(
        Command::new(root().join("src/ripple-asm/target/release/rlink"))
            .arg(root().join("runtime/crt0.pobj"))
            .arg(object)
            .arg(root().join("runtime/libruntime.par"))
            .args([
                "-f",
                "bf",
                "--standalone",
                "--bf-host",
                "--memory-banks",
                "16",
                "--bank-size",
                "64000",
                "-o",
            ])
            .arg(&bf),
    );
    let out = tool(
        Command::new(root().join("target/release/rbf"))
            .arg(bf)
            .args(["--jit", "--headless", "--timeout", "30", "--exit-code"]),
    );
    assert_eq!(String::from_utf8(out.stdout).unwrap(), expected);
}
