use super::*;

#[test]
fn cross_unit_private_symbols_multiple_exports_recursion_select_and_casts() {
    let a = parse(
        r#"
internal function @helper
function i16 @helper() { block 0:
 ret 17 }
function i16 @left() { block 0:
 %0 = call i16 func @helper()
 ret %0 }
function i16 @fact(i16 %0) {
 block 0:
 %1 = ule i1 %0, 1
 br %1, label 1, label 2
 block 1:
 ret 1
 block 2:
 %2 = sub i16 %0, 1
 %3 = call i16 func @fact(%2)
 %4 = mul i16 %0, %3
 ret %4
}
function i64 @extend(i8 %0) { block 0:
 %1 = sext i64 %0
 ret %1 }
function i8 @choose(i16 %0) { block 0:
 %1 = select i8 %0, 511, 256
 ret %1 }
function i16 @dead() { block 0:
 %0 = intrinsic i16 @unsupported()
 ret %0 }
"#,
    );
    let b = parse(
        r#"
internal function @helper
function i16 @helper() { block 0:
 ret 29 }
function i16 @right() { block 0:
 %0 = call i16 func @helper()
 ret %0 }
"#,
    );
    let artifact = compile(
        &[a, b],
        &options(&["left", "right", "fact", "extend", "choose"]),
    )
    .unwrap();
    assert_eq!(artifact.statistics.functions, 7);
    run(
        &artifact,
        vec![
            case("left", &[], 17),
            case("right", &[], 29),
            case("fact", &[5], 120),
            case("extend", &[255], u64::MAX),
            case("choose", &[0], 0),
            case("choose", &[1], 255),
        ],
    );
}

#[test]
fn cli_links_the_example_and_exports_requested_roots() {
    static ID: AtomicU64 = AtomicU64::new(0);
    let output = std::env::temp_dir().join(format!(
        "rcc-wasm-cli-{}-{}.wasm",
        std::process::id(),
        ID.fetch_add(1, Ordering::Relaxed)
    ));
    let status = Command::new(env!("CARGO_BIN_EXE_rcc-wasm"))
        .arg(concat!(env!("CARGO_MANIFEST_DIR"), "/examples/sum.ir"))
        .args(["--export", "sum", "-o"])
        .arg(&output)
        .output()
        .unwrap();
    assert!(
        status.status.success(),
        "{}",
        String::from_utf8_lossy(&status.stderr)
    );
    let bytes = fs::read(&output).unwrap();
    fs::remove_file(output).unwrap();
    run(
        &Artifact {
            bytes,
            statistics: Default::default(),
        },
        vec![case("sum", &[11], 55)],
    );
}
