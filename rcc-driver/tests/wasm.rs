//! One shared IR program, executed by Wasm, native code, and RVM.
use rcc_ir::target::Target;
use ripple_asm::{AssemblerOptions, Linker, Register, RippleAssembler};
use std::{fs, process::Command};

struct Fixture(std::path::PathBuf);
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
#[test]
fn loop_calls_and_phis_agree_across_three_targets() {
    let path = std::env::temp_dir().join(format!("rcc-wasm-differential-{}", std::process::id()));
    fs::create_dir_all(&path).unwrap();
    let f = Fixture(path);
    let mut module = rcc_ir::parse_module(include_str!("../../rcc-wasm/examples/sum.ir")).unwrap();
    let wasm = rcc_wasm::compile(&[module.clone()], &rcc_wasm::Options::default()).unwrap();
    assert_eq!(wasm.statistics.dispatch_functions, 0);
    let wasm_path = f.0.join("sum.wasm");
    fs::write(&wasm_path, wasm.bytes).unwrap();
    let result = Command::new(std::env::var_os("NODE").unwrap_or_else(|| "node".into()))
        .args(["-e", "const b=require('node:fs').readFileSync(process.argv[1]); const m=new WebAssembly.Module(b); if(WebAssembly.Module.imports(m).length)throw Error('unexpected imports'); console.log(new WebAssembly.Instance(m,{}).exports.main().toString());"])
        .arg(wasm_path).output().unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    assert_eq!(String::from_utf8_lossy(&result.stdout).trim(), "55");

    module.target = Target::Native;
    let exe = f.0.join("sum.native");
    rcc_native::compile_executable(&module, &exe).unwrap();
    assert_eq!(Command::new(exe).status().unwrap().code(), Some(55));

    module.target = Target::Ripple;
    let instructions = rcc_backend::lower_module_to_assembly_with_options(
        &module,
        rcc_backend::LoweringOptions {
            bank_size: 64000,
            ..Default::default()
        },
    )
    .unwrap();
    let asm = rcc_codegen::emit::emit_complete_program(instructions, true).unwrap();
    let assemble = |source: &str| {
        let mut options = AssemblerOptions::default();
        options.bank_size = 64000;
        options.max_immediate = 65535;
        options.memory_offset = 0;
        RippleAssembler::new(options).assemble(source).unwrap()
    };
    // Bootstrap and compiler-generated code are separate objects: the latter's
    // RCC link metadata describes only its own functions and data.
    let boot = assemble("_start:\n LI SB, 2\n LI SP, 1\n CALL main\n HALT\n");
    let program = assemble(&asm);
    let linked = Linker::new(64000).link(vec![boot, program]).unwrap();
    let mut vm = rvm::VM::with_options(64000, 64000 * 4, Some(f.0.join("disk.img")));
    vm.instructions = linked
        .instructions
        .iter()
        .map(|i| rvm::Instr::new(i.opcode, i.opcode, i.word1, i.word2, i.word3))
        .collect();
    vm.host_input_enabled = false;
    vm.state = rvm::vm::VMState::Running;
    vm.registers[Register::Pc as usize] = linked.entry_point as u16;
    for _ in 0..1000 {
        vm.run_batch(1024).unwrap();
        if matches!(vm.state, rvm::vm::VMState::Halted) {
            break;
        }
    }
    assert!(matches!(vm.state, rvm::vm::VMState::Halted));
    assert_eq!(vm.registers[Register::Rv0 as usize], 55);
}

#[test]
fn c_source_builds_to_wasm_and_matches_native_runtime_output() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let path = std::env::temp_dir().join(format!("rcc-wasm-source-{}", std::process::id()));
    fs::create_dir_all(&path).unwrap();
    let f = Fixture(path);
    let source = root.join("c-test/tests/core/wasm/test_wasm_runtime_core.c");
    for target in ["wasm", "native"] {
        let output = f.0.join(format!("program.{target}"));
        let result = Command::new(env!("CARGO_BIN_EXE_rcc"))
            .arg("compile")
            .arg(&source)
            .args(["--target", target, "-I"])
            .arg(root.join("runtime/include"))
            .arg("-o")
            .arg(&output)
            .output()
            .unwrap();
        assert!(
            result.status.success(),
            "{}",
            String::from_utf8_lossy(&result.stderr)
        );
        let result = if target == "wasm" {
            Command::new(std::env::var_os("NODE").unwrap_or_else(|| "node".into()))
                .arg(root.join("rcc-wasm/runner.mjs"))
                .arg(output)
                .output()
                .unwrap()
        } else {
            Command::new(output).output().unwrap()
        };
        assert!(
            result.status.success(),
            "{target}: {}",
            String::from_utf8_lossy(&result.stderr)
        );
        assert_eq!(
            String::from_utf8_lossy(&result.stdout),
            "YYYYYYY\n",
            "{target}"
        );
    }
}
