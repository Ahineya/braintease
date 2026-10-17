use rcc_preprocessor::Preprocessor;
use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};

static NEXT: AtomicUsize = AtomicUsize::new(0);

struct Headers(PathBuf);

impl Headers {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "rcc-includes-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(path.join("system")).unwrap();
        Self(path)
    }

    fn write(&self, name: &str, source: &str) {
        fs::write(self.0.join(name), source).unwrap();
    }

    fn process(&self, source: &str) -> anyhow::Result<String> {
        let mut pp = Preprocessor::new();
        pp.add_include_dir(self.0.join("system"));
        pp.process(source, self.0.join("main.c"))
    }
}

impl Drop for Headers {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.0).unwrap();
    }
}

#[test]
fn object_function_and_stringified_includes_expand_at_execution_time() {
    let h = Headers::new();
    h.write("first.h", "#define HEADER \"second.h\"\nint first;\n");
    h.write("second.h", "int second;\n");
    h.write("system/third.h", "int third;\n");
    let output = h
        .process(
            "#define INCFILE(name) name\n\
         #define HEADER \"first.h\"\n\
         #include INCFILE(HEADER)\n\
         #include HEADER\n\
         #define STRINGIFY_RAW(x) #x\n\
         #define STRINGIFY(x) STRINGIFY_RAW(x)\n\
         #define NAME second.h\n\
         #include STRINGIFY(NAME)\n\
         #define SYSTEM <third.h>\n\
         #define ALIAS SYSTEM\n\
         #include INCFILE(ALIAS)\n",
        )
        .unwrap();
    assert_eq!(output.matches("int first;").count(), 1, "{output}");
    assert_eq!(output.matches("int second;").count(), 2, "{output}");
    assert_eq!(output.matches("int third;").count(), 1, "{output}");
}

#[test]
fn inactive_includes_are_not_expanded_validated_or_opened() {
    let h = Headers::new();
    let output = h
        .process(
            "#define INCFILE(name) name\n\
         #ifdef MWCONFIG_FILE\n\
         #include INCFILE(MWCONFIG_FILE)\n\
         #endif\n\
         #if 0\n\
         #include INCFILE(too,many,args)\n\
         #include\n\
         #include <missing.h> trailing tokens\n\
         #include \"missing.h\"\n\
         #endif\n\
         int visible;\n",
        )
        .unwrap();
    assert_eq!(output.trim(), "int visible;");
}

#[test]
fn literal_headers_are_not_macro_expanded_and_keep_search_order() {
    let h = Headers::new();
    h.write("literal.h", "int local_header;\n");
    h.write("system/literal.h", "int system_header;\n");
    let output = h
        .process(
            "#define literal absent\n\
         #include /* before */ \"literal.h\" /* after */\n\
         #include <literal.h> // after\n",
        )
        .unwrap();
    assert!(output.contains("int local_header;"), "{output}");
    assert!(output.contains("int system_header;"), "{output}");
}

#[test]
fn active_includes_require_one_complete_header_name() {
    let h = Headers::new();
    h.write("ok.h", "int included;\n");
    for operand in [
        "",
        "UNKNOWN",
        "<>",
        "\"\"",
        "<ok.h",
        "\"ok.h\" junk",
        "<ok.h> junk",
        "\"ok.h\" \"ok.h\"",
        "BAD",
        "CONCAT",
    ] {
        let source = format!(
            "#define BAD \"ok.h\" junk\n#define CONCAT \"ok.h\" \"ok.h\"\n#include {operand}\n"
        );
        assert!(h.process(&source).is_err(), "accepted {operand:?}");
    }
    assert!(h
        .process("#include \"missing.h\"\n")
        .unwrap_err()
        .to_string()
        .contains("Cannot find include file"));
}
