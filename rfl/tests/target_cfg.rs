use rfl::{compile_to_ir_for_target, Target};
#[test]
fn cfg_selects_items_before_resolution_for_the_selected_target() {
    let source = r#"
        #[cfg(target_has = "host_math")]
        fn answer() -> i16 { 42 }
        #[cfg(not(target_has = "host_math"))]
        fn answer() -> i16 { 7 }
        #[cfg(all(target = "native", any(target_has = "host_math", target = "ripple")))]
        fn native_only() -> i16 { answer() }
        #[cfg(target = "ripple")]
        fn skipped_on_native() { missing_ripple_symbol(); }
        fn main() -> i16 { answer() }
    "#;
    assert!(compile_to_ir_for_target(source, Target::Native).is_ok());
    assert!(compile_to_ir_for_target(source, Target::Ripple).is_err());
}
#[test]
fn cfg_validates_unknown_capabilities_and_inactive_syntax() {
    for source in [
        r#"#[cfg(target_has = "host_mat")] fn main() {}"#,
        r#"#[cfg(not())] fn main() {}"#,
        r#"#[cfg(not(target = "native", target = "ripple"))] fn main() {}"#,
        r#"#[cfg(any(target = "native", target_has = "typo"))] fn main() {}"#,
        r#"#[cfg(target = "ripple")] fn broken( {} fn main() {}"#,
    ] {
        assert!(
            compile_to_ir_for_target(source, Target::Native).is_err(),
            "{source}"
        );
    }
}
#[test]
fn signed_minimum_literals_are_checked_after_applying_the_sign() {
    let source = "fn main() -> i16 { let a: i8 = -128; let b: i16 = -32768; b + (a as i16) }";
    for target in [Target::Ripple, Target::Native] {
        assert!(compile_to_ir_for_target(source, target).is_ok());
    }
    for source in [
        "fn main() { let x: i8 = -129; }",
        "fn main() { let x: i16 = -32769; }",
    ] {
        assert!(compile_to_ir_for_target(source, Target::Native).is_err());
    }
}

#[test]
fn cfg_composes_with_repr_methods_and_extern_declarations() {
    let source = r#"
        #[repr(C)]
        #[cfg(target = "native")]
        struct Point { x: c_int }
        #[cfg(target = "ripple")]
        #[repr(C)]
        struct Point { x: Missing }
        impl Point {
            #[cfg(target = "native")] fn answer(self) -> i16 { self.x }
            #[cfg(target = "ripple")] fn answer(self) -> i16 { unknown() }
        }
        extern "C" {
            #[cfg(target = "ripple")] fn missing(x: Missing);
        }
        fn main() -> i16 { Point { x: 42 }.answer() }
    "#;
    assert!(compile_to_ir_for_target(source, Target::Native).is_ok());
    assert!(compile_to_ir_for_target(source, Target::Ripple).is_err());
}
