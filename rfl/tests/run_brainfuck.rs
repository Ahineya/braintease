#[path = "support/fp.rs"]
mod support;

use std::process::{Command, Stdio};

#[test]
fn cli_runs_brainfuck_with_lite_telemetry_and_preserves_guest_status() {
    let fixture = support::Fixture::new(
        r#"fn main() -> i16 {
            let answer = for { a <- Some(20); b <- Some(22); yield a + b };
            println(answer);
            37
        }"#,
    );
    for (target, jit) in [("rbf", false), ("rbf", true), ("ripple", false)] {
        let mut command = Command::new(env!("CARGO_BIN_EXE_rfl"));
        command
            .arg("run")
            .arg(&fixture.0)
            .args(["--target", target]);
        if jit {
            command.arg("--jit");
        }
        let child = command
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .unwrap();
        let id = child.id();
        let output = child.wait_with_output().unwrap();
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert_eq!(
            output.status.code(),
            Some(37),
            "target={target}, jit={jit}: {stderr}"
        );
        assert_eq!(String::from_utf8(output.stdout).unwrap(), "Some(42)\n");
        if target == "rbf" {
            assert!(stderr.contains(" | Guest "), "jit={jit}: {stderr}");
            assert!(stderr.contains("(halted)"), "jit={jit}: {stderr}");
            let image_bytes: u64 = stderr
                .lines()
                .find_map(|line| {
                    line.trim()
                        .strip_prefix("Finished Brainfuck image: ")?
                        .strip_suffix(" bytes")?
                        .parse()
                        .ok()
                })
                .expect("runner reports the actual exported image size");
            // The old macro route produces about 283 MB for this fixture.
            // Keep a generous deterministic size budget, rather than a flaky
            // wall-time assertion, to guard compact export in the actual CLI.
            assert!(
                image_bytes < 20_000_000,
                "unexpectedly large BF image: {image_bytes}"
            );
        }
        for entry in std::fs::read_dir(support::root().join("target/rfl-run")).unwrap() {
            let path = entry.unwrap().path();
            let stem = path.file_stem().unwrap().to_string_lossy();
            assert!(
                !stem.ends_with(&format!("-{id}")),
                "left temporary artifact: {}",
                path.display()
            );
        }
    }
}

#[test]
fn native_jit_rejection_remains_explicit() {
    let output = Command::new(env!("CARGO_BIN_EXE_rfl"))
        .args(["run", "unused.rfl", "--target", "native", "--jit"])
        .output()
        .unwrap();
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("ahead-of-time compiled"));
}
