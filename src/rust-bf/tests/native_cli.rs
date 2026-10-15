use std::io::Write;
use std::process::{Command, Stdio};

fn output(args: &[&str]) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_bf"))
        .args(args)
        .output()
        .unwrap()
}

#[test]
fn fast_cli_writes_raw_bytes_and_preserves_wrapping() {
    for extra in [vec![], vec!["--no-optimize"], vec!["--verbose"]] {
        let mut args = vec!["--execute=-."];
        args.extend(extra);
        let result = output(&args);
        assert!(result.status.success());
        assert_eq!(result.stdout, [255]);
    }
    let result = output(&["-e", "<+>.<.", "--tape-size", "2"]);
    assert!(result.status.success());
    assert_eq!(result.stdout, [0, 1]);
}

#[test]
fn compatibility_modes_preserve_saturation_and_clamping() {
    for size in ["8", "16", "32"] {
        let result = output(&["--execute=-+.", "--no-wrap", "--cell-size", size]);
        assert!(result.status.success());
        assert_eq!(result.stdout, [1]);
    }
    let result = output(&["-e", "<>+<.", "--no-wrap-tape", "--tape-size", "2"]);
    assert!(result.status.success());
    assert_eq!(result.stdout, [0]);
    let code = format!("{}.", "+".repeat(300));
    let result = output(&["-e", &code, "--no-wrap", "--cell-size", "8"]);
    assert!(result.status.success());
    assert_eq!(result.stdout, [255]);
}

#[test]
fn malformed_code_and_zero_tapes_fail_explicitly() {
    for args in [
        vec!["-e", "["],
        vec!["-e", "]", "--cell-size", "8"],
        vec!["-e", "+", "--tape-size", "0"],
    ] {
        let result = output(&args);
        assert!(!result.status.success());
        assert!(String::from_utf8_lossy(&result.stderr).contains("Error:"));
        assert!(!String::from_utf8_lossy(&result.stderr).contains("panicked"));
    }
}

#[test]
fn input_bytes_and_eof_survive_native_output() {
    let mut child = Command::new(env!("CARGO_BIN_EXE_bf"))
        .args(["-e", ",.,.,."])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    child.stdin.take().unwrap().write_all(&[128, 255]).unwrap();
    let result = child.wait_with_output().unwrap();
    assert!(result.status.success());
    assert_eq!(result.stdout, [128, 255, 0]);
}

#[cfg(all(feature = "jit", any(target_arch = "aarch64", target_arch = "x86_64")))]
#[test]
fn jit_cli_executes_native_loops_and_rejects_incompatible_cell_modes() {
    let source = format!("{}[>+<--]>.", "+".repeat(4000));
    let reference = output(&["-e", &source]);
    let native = output(&["-e", &source, "--jit", "--stat"]);
    assert!(
        native.status.success(),
        "{}",
        String::from_utf8_lossy(&native.stderr)
    );
    assert_eq!(native.stdout, reference.stdout);
    let stats = String::from_utf8_lossy(&native.stderr);
    assert!(stats.contains("JIT: JitStats"), "{stats}");
    assert!(!stats.contains("native_dispatches: 0"), "{stats}");
    for extra in [
        vec!["--cell-size", "8"],
        vec!["--no-wrap"],
        vec!["--no-wrap-tape"],
        vec!["--emit-optimized"],
    ] {
        let mut args = vec!["-e", "+.", "--jit"];
        args.extend(extra);
        let result = output(&args);
        assert!(!result.status.success());
    }
}

#[cfg(not(all(feature = "jit", any(target_arch = "aarch64", target_arch = "x86_64"))))]
#[test]
fn jit_cli_fails_explicitly_without_a_native_backend() {
    let result = output(&["-e", "+.", "--jit"]);
    assert!(!result.status.success());
    assert!(result.stdout.is_empty());
    assert!(String::from_utf8_lossy(&result.stderr).contains("BF JIT requires"));
}
