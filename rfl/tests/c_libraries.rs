//! Library-only C imports must preserve translation-unit private symbols.
#[test]
fn private_c_functions_and_function_pointers_are_scoped_per_translation_unit() {
    let root = std::env::temp_dir().join(format!("rfl-c-libraries-{}", std::process::id()));
    std::fs::create_dir_all(&root).unwrap();
    std::fs::write(root.join("a.c"),"static int fail(void) {return 20;} static int (*callback)(void)=fail; int first(void) {return callback();}").unwrap();
    std::fs::write(root.join("b.c"),"static int fail(void) {return 22;} static int (*callback)(void)=fail; int second(void) {return callback();}").unwrap();
    std::fs::write(
        root.join("main.rfl"),
        r#"
        import {} from "./a.c";
        import {} from "./b.c";
        extern "C" {fn first()->c_int;fn second()->c_int;}
        fn main()->i16 {first()+second()}
    "#,
    )
    .unwrap();
    for target in [rfl::Target::Ripple, rfl::Target::Native] {
        rfl::compile_file_for_target(&root.join("main.rfl"), target).unwrap();
    }
    for run in [
        rfl::run_file(&root.join("main.rfl")),
        rfl::run_native_file(&root.join("main.rfl"), false),
    ] {
        let run = run.unwrap();
        assert_eq!(run.status, 42, "{}", run.stdout);
    }
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn public_c_globals_are_shared_and_tentative_definitions_coalesce() {
    let root = std::env::temp_dir().join(format!("rfl-c-shared-{}", std::process::id()));
    std::fs::create_dir_all(&root).unwrap();
    std::fs::write(
        root.join("a.c"),
        "int shared; extern int once; int first(void) {return shared+once;}",
    )
    .unwrap();
    std::fs::write(
        root.join("b.c"),
        "int shared; int once=2; int second(void) {shared=20;return 20;}",
    )
    .unwrap();
    std::fs::write(
        root.join("main.rfl"),
        r#"
        import {} from "./a.c"; import {} from "./b.c";
        extern "C" {fn first()->c_int; fn second()->c_int;}
        fn main()->i16 {let value=second();value+first()}
    "#,
    )
    .unwrap();
    for result in [
        rfl::run_file(&root.join("main.rfl")),
        rfl::run_native_file(&root.join("main.rfl"), false),
    ] {
        let result = result.unwrap();
        assert_eq!(result.status, 42, "{}", result.stdout);
    }
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn duplicate_public_c_definitions_still_report_an_error() {
    let root = std::env::temp_dir().join(format!("rfl-c-duplicates-{}", std::process::id()));
    std::fs::create_dir_all(&root).unwrap();
    std::fs::write(
        root.join("main.rfl"),
        r#"
        import {} from "./a.c"; import {} from "./b.c";
        fn main()->i16 {0}
    "#,
    )
    .unwrap();
    for (first, second, diagnostic) in [
        (
            "int public(void){return 1;}",
            "int public(void){return 2;}",
            "duplicate function `public`",
        ),
        (
            "int shared=1;",
            "int shared=2;",
            "duplicate global `shared`",
        ),
    ] {
        std::fs::write(root.join("a.c"), first).unwrap();
        std::fs::write(root.join("b.c"), second).unwrap();
        let error = rfl::compile_file(&root.join("main.rfl")).unwrap_err();
        assert!(error.to_string().contains(diagnostic), "{error}");
    }
    std::fs::remove_dir_all(root).unwrap();
}
