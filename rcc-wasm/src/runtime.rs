//! Compile the tiny freestanding runtime once per compiler process. This keeps
//! allocation/string loops in Wasm and reuses the frontends' C16 data ABI.
use rcc_ir::{ir::Module, target::Target};
use std::sync::OnceLock;
pub(crate) fn modules(browser: bool) -> Result<&'static [Module], String> {
    static PLAIN: OnceLock<Result<Vec<Module>, String>> = OnceLock::new();
    static BROWSER: OnceLock<Result<Vec<Module>, String>> = OnceLock::new();
    let modules = if browser { &BROWSER } else { &PLAIN };
    modules
        .get_or_init(|| {
            [
                ("wasm-heap", include_str!("../runtime/heap.c")),
                ("wasm-console", include_str!("../runtime/console.c")),
                ("wasm-strings", include_str!("../runtime/strings.c")),
                ("wasm-browser", include_str!("../runtime/browser.c")),
                ("wasm-effects", include_str!("../runtime/effects.c")),
                (
                    "wasm-browser-executor",
                    include_str!("../runtime/browser_executor.c"),
                ),
                ("wasm-softmath", include_str!("../../runtime/softmath.c")),
            ]
            .into_iter()
            .filter(|(name, _)| {
                browser
                    || !matches!(
                        *name,
                        "wasm-browser" | "wasm-effects" | "wasm-browser-executor"
                    )
            })
            .map(|(name, source)| {
                if browser && name == "wasm-heap" {
                    c_module(name, &format!("#define RIPPLE_WASM_BROWSER 1\n{source}"))
                } else {
                    c_module(name, source)
                }
            })
            .collect()
        })
        .as_ref()
        .map(Vec::as_slice)
        .map_err(Clone::clone)
}
fn c_module(name: &str, source: &str) -> Result<Module, String> {
    // This runtime is bundled into the compiler, not loaded from a checkout.
    // Softmath's optional VM acceleration is unavailable in the browser; keep
    // its software path and bundle its companion headers just like its C body.
    let source = if name == "wasm-softmath" {
        let header = include_str!("../../runtime/include/__float_accel.h").replace(
            "#include <limits.h>",
            include_str!("../../runtime/include/limits.h"),
        );
        std::borrow::Cow::Owned(format!(
            "#define RCC_SOFTWARE_FLOAT 1\n{}",
            source.replacen("#include \"include/__float_accel.h\"", &header, 1)
        ))
    } else {
        std::borrow::Cow::Borrowed(source)
    };
    let source = rcc_preprocessor::Preprocessor::new()
        .process(&source, std::path::PathBuf::from(format!("{name}.c")))
        .map_err(|e| format!("{name}: {e}"))?;
    let mut ast =
        rcc_frontend::Frontend::parse_source(&source).map_err(|e| format!("{name}: {e}"))?;
    let mut analyzer = rcc_frontend::SemanticAnalyzer::new();
    analyzer
        .analyze(&mut ast)
        .map_err(|e| format!("{name}: {e}"))?;
    let typed = rcc_frontend::type_translation_unit(&ast, analyzer.into_type_info())
        .map_err(|e| format!("{name}: {e}"))?;
    let mut module = rcc_frontend::TypedCodeGenerator::new(name.into())
        .generate(&typed)
        .map_err(|e| format!("{name}: {e}"))?;
    if name == "wasm-softmath" {
        module
            .functions
            .retain(|f| !matches!(f.name.as_str(), "__rcc_f64_floor" | "__rcc_f64_sqrt"));
    }
    module.target = Target::Wasm;
    Ok(module)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn embedded_softmath_is_self_contained_and_uses_no_vm_mailbox() {
        let module = c_module("wasm-softmath", include_str!("../../runtime/softmath.c")).unwrap();
        assert_eq!(module.target, Target::Wasm);
        assert!(module
            .functions
            .iter()
            .any(|f| f.name == "__rcc_f64_reduce_pi2"));
        assert!(!module
            .functions
            .iter()
            .any(|f| f.name.contains("float_accel") || f.name.contains("mmio")));
    }
}
