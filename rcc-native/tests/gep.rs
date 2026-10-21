//! Execute emitted code against an instrumented checked-address oracle. This
//! requires no guest SDK/runtime changes and exercises both runtime ceilings.
use rcc_ir::{ir::*, target::Target};
use std::{path::PathBuf, process::Command};

struct Fixture(PathBuf);
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn program() -> Module {
    let mut module = Module::new("gep-paths".into());
    module.target = Target::Native;
    let functions = [
        ("gep1", IrType::I16, IrType::I64),
        ("gep2", IrType::I32, IrType::I64),
        ("gep4", IrType::I64, IrType::I64),
        ("gep16", IrType::I16, IrType::I16),
        ("gep32", IrType::I16, IrType::I32),
    ];
    let mut main = IrBuilder::new();
    main.create_function("main".into(), IrType::I64);
    main.create_block(0).unwrap();
    let mut total = Value::Constant(0);
    for (name, element, index) in functions {
        let mut function = Function::new(name.into(), IrType::I64);
        function.parameters = vec![(1, IrType::I64), (2, index)];
        let mut block = BasicBlock::new(0);
        block.instructions.push(Instruction::GetElementPtr {
            result: 3,
            ptr: Value::Temp(1),
            indices: vec![Value::Temp(2)],
            result_type: IrType::FatPtr(Box::new(element.clone())),
            source_element_type: element,
            is_struct_field: false,
        });
        block
            .instructions
            .push(Instruction::Return(Some(Value::Temp(3))));
        function.add_block(block);
        module.add_function(function);
        let result = main
            .build_call(
                Value::Function(name.into()),
                vec![Value::Constant(0), Value::Constant(0)],
                IrType::I64,
            )
            .unwrap()
            .unwrap();
        total = Value::Temp(
            main.build_binary(IrBinaryOp::Add, total, Value::Temp(result), IrType::I64)
                .unwrap(),
        );
        // Keep a callable, non-inlined entry for the instrumented C harness.
        total = Value::Temp(
            main.build_binary(
                IrBinaryOp::Add,
                total,
                Value::Function(name.into()),
                IrType::I64,
            )
            .unwrap(),
        );
    }
    main.build_return(Some(total)).unwrap();
    module.add_function(main.finish_function().unwrap());
    module
}

#[test]
fn same_bank_arithmetic_bypasses_helpers_and_other_cases_preserve_checks() {
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let fixture =
        Fixture(std::env::temp_dir().join(format!("native-gep-{}-{stamp}", std::process::id(),)));
    std::fs::create_dir_all(&fixture.0).unwrap();
    let object = fixture.0.join("probe.o");
    std::fs::write(&object, rcc_native::compile_object(&program()).unwrap()).unwrap();
    let executable = fixture.0.join("probe");
    let source = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/gep-reference.c");
    let mut linker = Command::new("cc");
    linker
        .args(["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"])
        .arg(source)
        .arg(object)
        .arg("-o")
        .arg(&executable);
    if cfg!(target_os = "macos") {
        let sdk = Command::new("xcrun")
            .args(["--sdk", "macosx", "--show-sdk-path"])
            .output()
            .unwrap();
        assert!(sdk.status.success());
        let sdk = String::from_utf8(sdk.stdout).unwrap();
        linker.arg("-isysroot").arg(sdk.trim());
    }
    let linked = linker.output().unwrap();
    assert!(
        linked.status.success(),
        "{}",
        String::from_utf8_lossy(&linked.stderr)
    );

    for banks in ["256", "65536"] {
        let output = Command::new(&executable)
            .env("GEP_BANKS", banks)
            .env_remove("GEP_FAULT")
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "banks={banks}: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        assert!(String::from_utf8_lossy(&output.stdout).contains("GEP differential paths passed"));
        let faults = [
            "bad-offset",
            "bad-offset-recovery",
            "bad-bank",
            "underflow",
            "overflow",
            "min-delta",
            "max-delta",
            "high-carrier",
        ];
        for fault in faults {
            let output = Command::new(&executable)
                .env("GEP_BANKS", banks)
                .env("GEP_FAULT", fault)
                .output()
                .unwrap();
            assert_eq!(
                output.status.code(),
                Some(86),
                "{banks}/{fault}: {output:?}"
            );
            assert!(String::from_utf8_lossy(&output.stderr).contains("checked-gep:"));
        }
    }

    // The same lowering must verify and emit for the separate x64 runtime.
    let object =
        rcc_native::compile_freestanding_object(&program(), "x86_64-unknown-none").unwrap();
    assert_eq!(&object[..6], b"\x7fELF\x02\x01");
}
