use super::Analysis;

#[test]
fn extraction_methods_complete_and_navigate_with_concrete_output_types() {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/extraction-ide.rfl");
    for (module, name, initializer) in [
        ("option", "Option", "Some(42)"),
        ("result", "Result", "Ok(42)"),
    ] {
        let ty = if name == "Option" {
            "Option<i16>"
        } else {
            "Result<i16,unit>"
        };
        let source = format!(
            r#"
            import {{ {name} }} from "std/{module}";
            fn main()->i16 {{let value:{ty} = {initializer};value.expect("value missing")}}
        "#
        );
        let doc = Analysis::open(Some(&path), &source);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
        let at = source.find(".expect(").unwrap() + 1;
        let (file, span) = doc.definition(at).unwrap();
        let file = file.unwrap();
        assert!(file.ends_with(format!("std/{module}.rfl")));
        let library = std::fs::read_to_string(file).unwrap();
        assert_eq!(&library[span.start..span.end], "expect");
        let signature = doc
            .signature(source.find("\"value missing\"").unwrap())
            .unwrap()
            .0;
        assert_eq!(signature, "fn(String) -> i16");
        assert!(!doc
            .info
            .call_sigs
            .iter()
            .any(|s| s.label.contains("__expect_value")));
        let editing = source.replace("value.expect(\"value missing\")", "value.un");
        let doc = Analysis::open(Some(&path), &editing);
        let items = doc.completions(editing.find("value.un").unwrap() + 8);
        for method in ["unwrap", "expect"] {
            assert!(
                items.iter().any(|item| item.label == method),
                "missing {method}"
            );
        }
    }
}
