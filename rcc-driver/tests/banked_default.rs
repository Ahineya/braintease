use std::{fs, process::Command};

#[test]
fn source_and_ir_lowering_default_to_banked_globals_with_explicit_legacy_opt_out() {
    let dir = std::env::temp_dir().join(format!("rcc-global-default-{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    let source = dir.join("global.c");
    fs::write(
        &source,
        "int value = 7; int *address(void) { return &value; }\n",
    )
    .unwrap();
    let ir = dir.join("global.ir");
    let emitted = Command::new(env!("CARGO_BIN_EXE_rcc"))
        .args(["compile", source.to_str().unwrap(), "--emit-ir"])
        .output()
        .unwrap();
    assert!(emitted.status.success(), "{:?}", emitted);
    fs::write(&ir, emitted.stdout).unwrap();
    for from_ir in [false, true] {
        for (flag, banked) in [
            (None, true),
            (Some("--banked-globals"), true),
            (Some("--banked-globals=false"), false),
        ] {
            let output = dir.join("global.asm");
            let mut cmd = Command::new(env!("CARGO_BIN_EXE_rcc"));
            if from_ir {
                cmd.args(["generate-asm", "-i"]).arg(&ir);
            } else {
                cmd.arg("compile").arg(&source);
            }
            cmd.arg("-o").arg(&output);
            if let Some(flag) = flag {
                cmd.arg(flag);
            }
            let result = cmd.output().unwrap();
            assert!(result.status.success(), "{:?}", result);
            let asm = fs::read_to_string(output).unwrap();
            assert!(asm.contains(&format!("\"banked_data\":{banked}")), "{asm}");
            assert_eq!(asm.contains("%data_bank(value+0)"), banked);
        }
    }
    fs::remove_dir_all(dir).unwrap();
}
