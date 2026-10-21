//! Execute emitted reads against an independent, instrumented memory oracle.
use rcc_ir::{ir::*, target::Target};
use std::{path::PathBuf, process::Command};

fn program() -> Module {
    let mut m = Module::new("read-view".into());
    m.target = Target::Native;
    let mut main = IrBuilder::new();
    main.create_function("main".into(), IrType::I64);
    main.create_block(0).unwrap();
    let mut tokens = Value::Constant(0);
    for (name, ty, count, volatile, barrier, gep) in [
        ("pair1", IrType::I16, 2, false, false, false),
        ("pair2", IrType::I32, 2, false, false, false),
        ("pair4", IrType::I64, 2, false, false, false),
        ("single", IrType::I16, 1, false, false, false),
        ("volatile", IrType::I16, 2, true, false, false),
        ("storebarrier", IrType::I16, 4, false, true, false),
        ("geppair", IrType::I16, 2, false, false, true),
        ("many", IrType::I16, 16, false, false, true),
    ] {
        let mut f = Function::new(name.into(), IrType::I64);
        f.parameters = vec![(1, IrType::I64), (2, IrType::I64)];
        let mut block = BasicBlock::new(0);
        let mut sum = Value::Constant(0);
        let mut id = 3;
        for i in 0..count {
            if barrier && i == 2 {
                block.instructions.push(Instruction::Store {
                    ptr: Value::Temp(1),
                    value: Value::Constant(7),
                    value_type: IrType::I16,
                    volatile: false,
                });
            }
            let ptr = if gep && i != 0 {
                block.instructions.push(Instruction::GetElementPtr {
                    result: id,
                    ptr: Value::Temp(1),
                    indices: vec![Value::Constant(i as i64)],
                    result_type: IrType::FatPtr(Box::new(IrType::I16)),
                    source_element_type: IrType::I16,
                    is_struct_field: false,
                });
                id += 1;
                Value::Temp(id - 1)
            } else {
                Value::Temp(1 + i % 2)
            };
            block.instructions.push(Instruction::Load {
                result: id,
                ptr,
                result_type: ty.clone(),
                volatile,
            });
            let loaded = Value::Temp(id);
            id += 1;
            block.instructions.push(Instruction::Binary {
                result: id,
                op: IrBinaryOp::Add,
                lhs: sum,
                rhs: loaded,
                result_type: IrType::I64,
            });
            sum = Value::Temp(id);
            id += 1;
        }
        block.instructions.push(Instruction::Return(Some(sum)));
        f.add_block(block);
        m.add_function(f);
        tokens = Value::Temp(
            main.build_binary(
                IrBinaryOp::Add,
                tokens,
                Value::Function(name.into()),
                IrType::I64,
            )
            .unwrap(),
        );
    }
    main.build_return(Some(tokens)).unwrap();
    m.add_function(main.finish_function().unwrap());
    m
}

// Isolate environment selection from concurrently running Rust tests.
#[test]
#[ignore = "subprocess compiler entry for differential test"]
fn emit() {
    let output = PathBuf::from(std::env::var_os("READ_VIEW_OBJECT").unwrap());
    let result = rcc_native::compile_object(&program());
    if std::env::var("READ_VIEW_INVALID").is_ok() {
        assert!(result.unwrap_err().contains("RCC_NATIVE_MEMORY"));
    } else {
        std::fs::write(output, result.unwrap()).unwrap();
        let mut app = program();
        app.functions
            .iter_mut()
            .find(|f| f.name == "main")
            .unwrap()
            .parameters = vec![(9999, IrType::I64)];
        let path =
            PathBuf::from(std::env::var_os("READ_VIEW_OBJECT").unwrap()).with_extension("fxe");
        rcc_native::compile_application(&[app], &path).unwrap();
        let bytes = std::fs::read(path).unwrap();
        let (manifest, _, _) = rcc_ir::native_image::decode(&bytes).unwrap();
        let helper = manifest
            .helpers
            .iter()
            .find(|h| h.symbol == "rcc_native_memview_v1");
        assert_eq!(
            helper.map(|h| h.arity),
            (std::env::var("RCC_NATIVE_MEMORY").unwrap() == "read-v1").then_some(3)
        );
        if std::env::var("RCC_NATIVE_MEMORY").unwrap() == "read-v1" {
            assert!(
                rcc_native::compile_freestanding_object(&program(), "x86_64-unknown-none")
                    .unwrap_err()
                    .contains("freestanding read-v1 runtime support is pending")
            );
        }
    }
}

#[test]
fn generated_reads_preserve_values_polls_denial_and_barriers() {
    let dir = std::env::temp_dir().join(format!(
        "native-memview-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    std::fs::create_dir_all(&dir).unwrap();
    struct Cleanup(PathBuf);
    impl Drop for Cleanup {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    let _cleanup = Cleanup(dir.clone());
    for mode in ["checked", "read-v1", "invalid"] {
        let object = dir.join(format!("{mode}.o"));
        let mut compiler = Command::new(std::env::current_exe().unwrap());
        compiler
            .args(["--exact", "emit", "--ignored", "--nocapture"])
            .env("READ_VIEW_OBJECT", &object)
            .env("RCC_NATIVE_MEMORY", mode);
        if mode == "invalid" {
            compiler.env("READ_VIEW_INVALID", "1");
        }
        let out = compiler.output().unwrap();
        assert!(
            out.status.success(),
            "{mode}: {} {}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        );
        if mode == "invalid" {
            continue;
        }
        // Default/off objects must not import even the acquisition query.
        let bytes = std::fs::read(&object).unwrap();
        assert_eq!(
            bytes
                .windows(b"rcc_native_memview_v1".len())
                .any(|w| w == b"rcc_native_memview_v1"),
            mode == "read-v1"
        );
        let exe = dir.join(mode);
        let mut cc = Command::new("cc");
        cc.args(["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"])
            .arg(
                PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                    .join("tests/fixtures/memview-reference.c"),
            )
            .arg(&object)
            .arg("-o")
            .arg(&exe);
        if mode == "read-v1" {
            cc.arg("-DFAST=1");
        }
        if cfg!(target_os = "macos") {
            let sdk = Command::new("xcrun")
                .args(["--sdk", "macosx", "--show-sdk-path"])
                .output()
                .unwrap();
            cc.arg("-isysroot")
                .arg(String::from_utf8(sdk.stdout).unwrap().trim());
        }
        let out = cc.output().unwrap();
        assert!(
            out.status.success(),
            "{}",
            String::from_utf8_lossy(&out.stderr)
        );
        let out = Command::new(&exe).output().unwrap();
        assert!(
            out.status.success(),
            "{mode}: {} {}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        );
        assert!(String::from_utf8_lossy(&out.stdout).contains("read-view differential passed"));
        for fault in ["offset", "bank", "last-cell", "carrier"] {
            let out = Command::new(&exe)
                .env("VIEW_FAULT", fault)
                .output()
                .unwrap();
            assert_eq!(out.status.code(), Some(86), "{mode}/{fault}: {out:?}");
            assert!(String::from_utf8_lossy(&out.stderr).contains("checked-load"));
        }
    }
}
