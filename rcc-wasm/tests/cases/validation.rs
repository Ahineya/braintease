use super::*;

#[test]
fn unsupported_or_invalid_ir_is_an_error() {
    let bodies = [
        (
            "function i16 @main() { block 0:
 %0 = call i16 func @missing()
 ret %0 }",
            "no runtime binding",
        ),
        (
            "function i16 @main() { block 0:
 br label 99 }",
            "unknown block",
        ),
        (
            "function i16 @main() { block 0:
 ret %42 }",
            "undefined IR temporary",
        ),
        (
            "function i16 @main() { block 0:
 %0 = phi i16 [1, label 0]
 ret %0 }",
            "phi",
        ),
        (
            "function i16 @main() { block 0:
 ret 1
 ret 2 }",
            "after a terminator",
        ),
        (
            "function i16 @main() { block 0:
 %0 = add i16 1, 2
 %0 = add i16 3, 4
 ret %0 }",
            "duplicate IR temporary",
        ),
        (
            "function i16 @main() { block 0:
 ret void }",
            "return",
        ),
        (
            "function i16 @main() { block 0:
 %0 = intrinsic i16 @unsupported()
 ret %0 }",
            "unknown Wasm intrinsic",
        ),
        (
            "function i16 @main() { block 0:
 asm \"HALT\"
 ret 0 }",
            "not implemented",
        ),
    ];
    for (body, error) in bodies {
        assert!(
            compile(&[parse(body)], &Options::default())
                .unwrap_err()
                .contains(error),
            "{body}"
        );
    }
    let mut m = parse(
        "function i16 @main() { block 0:
 ret 0 }",
    );
    m.target = Target::Ripple;
    assert!(compile(&[m], &Options::default())
        .unwrap_err()
        .contains("not compiled for wasm"));
    let m = parse(
        "function i16 @main() { block 0:
 ret 0 }",
    );
    assert!(compile(&[m.clone()], &options(&[]))
        .unwrap_err()
        .contains("export root"));
    assert!(compile(&[m.clone()], &options(&["missing"]))
        .unwrap_err()
        .contains("no public definition"));
    assert!(compile(&[m], &options(&["main", "main"]))
        .unwrap_err()
        .contains("duplicate Wasm export"));
}

#[test]
fn zero_initialized_wasm_locals_cannot_hide_invalid_ssa() {
    for body in [
        r#"function i16 @main() {
 block 0:
  %1 = add i16 %0, 1
  %0 = add i16 2, 3
  ret %1
}"#,
        r#"function i16 @main() {
 block 0: br 1, label 1, label 2
 block 1:
  %0 = add i16 1, 2
  br label 2
 block 2: ret %0
}"#,
        r#"function i16 @main() {
 block 0: br 1, label 1, label 2
 block 1:
  %0 = add i16 1, 2
  br label 3
 block 2: br label 3
 block 3:
  %1 = phi i16 [%0, label 1], [%0, label 2]
  ret %1
}"#,
    ] {
        assert!(compile(&[parse(body)], &Options::default())
            .unwrap_err()
            .contains("does not dominate"));
    }
}
