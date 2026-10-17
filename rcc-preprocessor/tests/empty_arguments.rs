use rcc_preprocessor::Preprocessor;
use std::path::PathBuf;

fn expand(source: &str) -> String {
    Preprocessor::new().process(source, PathBuf::from("empty.c")).unwrap()
}

#[test]
fn empty_arguments_preserve_macro_arity_and_placemarkers() {
    for (source, expected) in [
        ("#define Z() zero\nZ()\n", "zero"),
        ("#define V(x) value\nV()\n", "value"),
        ("#define S(x) #x\nS( )\n", "\"\""),
        ("#define P(a,b) a##b\nP(left,)\n", "left"),
        ("#define P(a,b) a##b\nP(,right)\n", "right"),
        ("#define S(a,b) #a #b\nS(,)\n", "\"\" \"\""),
        ("#define P(x,...) #x #__VA_ARGS__\nP(,)\n", "\"\" \"\""),
    ] {
        assert_eq!(expand(source).trim(), expected);
    }
}

#[test]
fn genuinely_missing_or_extra_arguments_still_fail() {
    for source in ["#define P(a,b) a\nP()\n", "#define Z() zero\nZ(,)\n"] {
        assert!(Preprocessor::new().process(source, PathBuf::from("bad.c")).is_err());
    }
}
