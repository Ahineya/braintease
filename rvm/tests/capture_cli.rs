use rvm::Instr;
use std::{
    fs,
    path::{Path, PathBuf},
    process::{Command, Output},
    sync::atomic::{AtomicU64, Ordering},
};
static ID: AtomicU64 = AtomicU64::new(0);
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "rvm-cli-{}-{}",
            std::process::id(),
            ID.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&path).unwrap();
        Self(path)
    }
    fn run(&self, args: &[&str]) -> Output {
        Command::new(env!("CARGO_BIN_EXE_rvm"))
            .arg(self.0.join("program.bin"))
            .args(["--headless", "--memory", "128000", "--disk"])
            .arg(self.0.join("disk.img"))
            .args(args)
            .current_dir(&self.0)
            .output()
            .unwrap()
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn instruction(opcode: u8, a: u16, b: u16, c: u16) -> Instr {
    Instr {
        opcode,
        word0: opcode,
        word1: a,
        word2: b,
        word3: c,
    }
}
fn store(code: &mut Vec<Instr>, addr: u16, value: u16) {
    code.extend([
        instruction(0x0e, 8, value, 0),
        instruction(0x0e, 9, addr, 0),
        instruction(0x12, 8, 0, 9),
    ]);
}
fn write_binary(path: &Path, code: &[Instr]) {
    let mut bytes = b"RLINK".to_vec();
    bytes.extend(64000u16.to_le_bytes());
    bytes.extend(0u32.to_le_bytes());
    bytes.extend((code.len() as u32).to_le_bytes());
    for i in code {
        bytes.extend([i.opcode, i.word0]);
        for w in [i.word1, i.word2, i.word3] {
            bytes.extend(w.to_le_bytes());
        }
    }
    bytes.extend(0u32.to_le_bytes());
    fs::write(path, bytes).unwrap();
}
fn scene(f: &Fixture) {
    let mut code = vec![instruction(0x0e, ripple_asm::Register::Rv0 as u16, 77, 0)];
    store(&mut code, 16, 0x0201);
    store(&mut code, 6, 3);
    store(&mut code, 34, 0xf800);
    store(&mut code, 35, 0x07e0);
    store(&mut code, 9, 1);
    code.push(instruction(0, 0, 0, 0));
    write_binary(&f.0.join("program.bin"), &code);
}
#[test]
fn cli_captures_presented_frame_and_ignores_unfinished_return_register() {
    let f = Fixture::new();
    scene(&f);
    let result = f.run(&[
        "--screenshot",
        "frame.png",
        "--raw-frame",
        "frame.rgb565",
        "--report",
        "report.json",
        "--exit-code",
    ]);
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    assert_eq!(
        fs::read(f.0.join("frame.rgb565")).unwrap(),
        vec![0, 248, 224, 7]
    );
    let report: serde_json::Value =
        serde_json::from_slice(&fs::read(f.0.join("report.json")).unwrap()).unwrap();
    assert_eq!(report["success"], true);
    assert_eq!(report["frame"], 1);
    assert_eq!(report["tick"], 0);
    assert_eq!(report["width"], 2);
    assert_eq!(report["height"], 1);
    assert_eq!(report["stop_reason"], "captured_frame");
    assert_eq!(report["binary_sha256"].as_str().unwrap().len(), 64);
    let result = f.run(&["--exit-code"]);
    assert_eq!(result.status.code(), Some(77));
}
#[test]
fn early_halt_and_budget_failure_write_reports_and_no_screenshot() {
    let f = Fixture::new();
    scene(&f);
    let result = f.run(&[
        "--screenshot",
        "missing.png",
        "--capture-frame",
        "2",
        "--report",
        "halt.json",
    ]);
    assert!(!result.status.success());
    assert!(!f.0.join("missing.png").exists());
    let result = f.run(&[
        "--screenshot",
        "budget.png",
        "--max-instructions",
        "0",
        "--report",
        "nested/budget.json",
    ]);
    assert!(!result.status.success());
    assert!(!f.0.join("budget.png").exists());
    let report: serde_json::Value =
        serde_json::from_slice(&fs::read(f.0.join("nested/budget.json")).unwrap()).unwrap();
    assert_eq!(report["success"], false);
    assert_eq!(report["instructions"], 0);
    assert!(report["stop_reason"]
        .as_str()
        .unwrap()
        .contains("Instruction limit"));
}
#[test]
fn invalid_display_initialization_cannot_report_a_successful_capture() {
    let f = Fixture::new();
    let mut code = Vec::new();
    store(&mut code, 6, 3);
    code.push(instruction(0, 0, 0, 0));
    write_binary(&f.0.join("program.bin"), &code);
    let result = f.run(&["--screenshot", "invalid.png", "--report", "invalid.json"]);
    assert!(!result.status.success());
    assert!(!f.0.join("invalid.png").exists());
    assert!(String::from_utf8_lossy(&result.stderr).contains("nonzero"));
}
