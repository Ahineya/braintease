use std::{
    fs,
    path::PathBuf,
    process::{Command, Output},
};

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!("rcc-output-{}", std::process::id()));
        fs::create_dir_all(&path).unwrap();
        fs::write(path.join("sample.c"),
            "#define VALUE 42\nstatic int get_value(void) { return VALUE; }\nint main(void) { return get_value(); }\n").unwrap();
        Self(path)
    }
    fn compile(&self, input: &str, flags: &[&str]) -> Output {
        Command::new(env!("CARGO_BIN_EXE_rcc"))
            .arg("compile")
            .arg(self.0.join(input))
            .arg("-o")
            .arg(self.0.join("sample.asm"))
            .args(flags)
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

#[test]
fn ir_output_modes_and_opt_in_timings_keep_their_stream_contracts() {
    let f = Fixture::new();
    for target in ["ripple", "native"] {
        let plain = f.compile("sample.c", &["--emit-ir", "--target", target]);
        let timed = f.compile("sample.c", &["--emit-ir", "--target", target, "--timings"]);
        assert!(
            plain.status.success(),
            "{}",
            String::from_utf8_lossy(&plain.stderr)
        );
        assert!(
            timed.status.success(),
            "{}",
            String::from_utf8_lossy(&timed.stderr)
        );
        assert_eq!(plain.stdout, timed.stdout);
        rcc_ir::parse_module(std::str::from_utf8(&timed.stdout).unwrap()).unwrap();
        assert!(!String::from_utf8_lossy(&plain.stderr).contains("rcc-timing"));
        let diagnostics = String::from_utf8_lossy(&timed.stderr);
        for stage in [
            "preprocess",
            "parse",
            "semantic",
            "type",
            "ir_generate",
            "ir_output",
            "total",
        ] {
            assert!(
                diagnostics.contains(&format!("rcc-timing {stage} ")),
                "{diagnostics}"
            );
        }
    }
    let emitted = f.compile("sample.c", &["--emit-ir"]);
    let saved = f.compile(
        "sample.c",
        &[
            "--print-ir",
            "--save-ir",
            "--ir-output",
            "saved.ir",
            "--timings",
        ],
    );
    assert!(
        saved.status.success(),
        "{}",
        String::from_utf8_lossy(&saved.stderr)
    );
    assert_eq!(fs::read(f.0.join("saved.ir")).unwrap(), emitted.stdout);
    assert!(String::from_utf8_lossy(&saved.stdout)
        .contains(std::str::from_utf8(&emitted.stdout).unwrap()));
    assert!(String::from_utf8_lossy(&saved.stderr).contains("rcc-timing ripple_lower_emit "));
    let from_ir = f.compile("saved.ir", &["--emit-ir", "--timings"]);
    assert!(from_ir.status.success());
    assert_eq!(from_ir.stdout, emitted.stdout);
    assert!(String::from_utf8_lossy(&from_ir.stderr).contains("rcc-timing read_parse_ir "));
    let ordinary = f.compile("sample.c", &[]);
    assert!(ordinary.status.success());
    assert!(!String::from_utf8_lossy(&ordinary.stdout).contains("=== IR Output ==="));
    assert!(!String::from_utf8_lossy(&ordinary.stderr).contains("rcc-timing"));
    assert!(!f.0.join("sample.ir").exists());
    fs::write(f.0.join("bad.c"), "#define PAIR(a,b) a+b\nPAIR(1)\n").unwrap();
    let failure = f.compile("bad.c", &["--emit-ir", "--timings"]);
    assert!(!failure.status.success());
    let error = String::from_utf8_lossy(&failure.stderr);
    assert!(error.contains("expects 2 arguments, got 1"));
    assert!(error.contains("rcc-timing preprocess "));
    assert!(error.contains("rcc-timing total "));
}
