use rcc_preprocessor::Preprocessor;
use std::path::PathBuf;

#[test]
fn phase_two_splices_strings_identifiers_macros_and_comments() {
    let source = "#define N 1 + \\\n2\nconst char *s = \"ab\\\ncd\";\nint va\\\r\nlue = N;\n// hidden \\\n#error hidden too\nint visible;\n";
    let out = Preprocessor::new().process(source, PathBuf::from("splice.c")).unwrap();
    assert!(out.contains("\"abcd\""), "{out}");
    assert!(out.contains("value"), "{out}");
    assert!(out.contains("1 + 2"), "{out}");
    assert!(out.contains("visible"), "{out}");
    assert!(!out.contains("hidden"), "{out}");
}
