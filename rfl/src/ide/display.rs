//! Render linked type names at the editor boundary, preserving fact identities.
use super::Analysis;

impl Analysis {
    pub(super) fn display_type(&self, text: &str) -> String {
        let mut output = String::with_capacity(text.len());
        let mut start = None;
        for (at, ch) in text
            .char_indices()
            .chain(std::iter::once((text.len(), '\0')))
        {
            if ch.is_alphanumeric() || matches!(ch, '_' | '$') {
                start.get_or_insert(at);
                continue;
            }
            if let Some(begin) = start.take() {
                let name = &text[begin..at];
                let display = self
                    .info
                    .display_names
                    .get(name)
                    .map(String::as_str)
                    .or_else(|| name.strip_prefix("$local$"))
                    .or_else(|| name.strip_prefix("$std$"))
                    .unwrap_or(name);
                output.push_str(display);
            }
            if at < text.len() {
                output.push(ch);
            }
        }
        output
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    #[test]
    fn nested_types_and_exact_names_use_source_aliases() {
        let mut analysis = Analysis::open(None, "fn main() {}");
        analysis.info.display_names.extend([
            ("m1$Value".into(), "Left".into()),
            ("m12$Value".into(), "Right".into()),
        ]);
        assert_eq!(
            analysis.display_type(
                "fn(&mut m1$Value, [m12$Value; 2]) -> Async<Result<Vec<m1$Value>, $local$Error>>"
            ),
            "fn(&mut Left, [Right; 2]) -> Async<Result<Vec<Left>, Error>>"
        );
        assert_eq!(analysis.display_type("m1$ValueSuffix"), "m1$ValueSuffix");
        assert_eq!(
            analysis.display_type("type $std$Printable"),
            "type Printable"
        );
    }

    struct Fixture(PathBuf);
    impl Fixture {
        fn new(name: &str) -> Self {
            let root =
                std::env::temp_dir().join(format!("rfl-display-{name}-{}", std::process::id()));
            std::fs::create_dir_all(&root).unwrap();
            Self(root)
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn aliases_disambiguate_types_without_breaking_member_resolution() {
        let fixture = Fixture::new("aliases");
        for (file, method) in [("left", "left_only"), ("right", "right_only")] {
            std::fs::write(fixture.0.join(format!("{file}.rfl")), format!(
                "export struct Value {{ n: i16 }}\nexport fn make() -> Value {{ Value {{ n: 1 }} }}\nimpl Value {{ export fn {method}(&self) -> i16 {{ self.n }} }}\n"
            )).unwrap();
        }
        let source = "import { Value as Left, make as make_left } from \"./left\";\nimport { Value as Right, make as make_right } from \"./right\";\nfn main() { let left = make_left(); let right = make_right(); println(left.left_only()); println(right.right_only()); let nested = Some([left]); }\n";
        let analysis = Analysis::open(Some(&fixture.0.join("main.rfl")), source);
        assert!(
            analysis.diagnostics.is_empty(),
            "{:?}",
            analysis
                .diagnostics
                .iter()
                .map(|e| &e.message)
                .collect::<Vec<_>>()
        );
        let hints = analysis.inlay_hints();
        for (name, expected) in [
            ("left", ": Left"),
            ("right", ": Right"),
            ("nested", ": Option<Vec<Left>>"),
        ] {
            let at = source.find(&format!("let {name}")).unwrap() + 4 + name.len();
            assert!(hints.contains(&(at, expected.to_string())), "{hints:?}");
        }
        let at = source.find("make_left()").unwrap() + "make_left(".len();
        assert_eq!(analysis.signature(at).unwrap().0, "fn() -> Left");
        let at = source.find("left.left_only").unwrap();
        assert_eq!(analysis.hover(at).unwrap(), "Left");
        let items = analysis.completions(at + "left.".len());
        assert!(items.iter().any(|item| item.label == "left_only"));
        assert!(!items.iter().any(|item| item.label == "right_only"));
        assert!(items.iter().all(|item| !item.detail.contains('$')));
        // The compiler-facing receiver identity is deliberately still qualified.
        assert!(analysis
            .info
            .expr_types
            .iter()
            .any(|expr| expr.ty.contains("m1$Value")));
    }

    #[test]
    fn original_name_wins_when_both_original_and_alias_are_imported() {
        let fixture = Fixture::new("original");
        std::fs::write(
            fixture.0.join("lib.rfl"),
            "export struct Value { n: i16 } export fn make() -> Value { Value { n: 1 } }",
        )
        .unwrap();
        let source = "import { Value, Value as Alias, make } from \"./lib\"; fn main() { let value = make(); println(value.n); }";
        let analysis = Analysis::open(Some(&fixture.0.join("main.rfl")), source);
        assert!(analysis.diagnostics.is_empty());
        assert!(analysis
            .inlay_hints()
            .iter()
            .any(|(_, hint)| hint == ": Value"));
    }

    #[test]
    fn http_server_hints_hide_internal_module_names() {
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("examples/http-server.rfl");
        let source = std::fs::read_to_string(&path).unwrap();
        let analysis = Analysis::open(Some(&path), &source);
        assert!(
            analysis.diagnostics.is_empty(),
            "{:?}",
            analysis
                .diagnostics
                .iter()
                .map(|e| &e.message)
                .collect::<Vec<_>>()
        );
        let hints = analysis.inlay_hints();
        for (name, expected) in [
            ("network", ": Network"),
            ("reactor", ": NetworkReactor"),
            ("lease", ": Async<Result<NetworkConfig, NetError>>"),
            ("config", ": NetworkConfig"),
            ("server", ": HttpServer"),
            ("service", ": Async<Result<unit, NetError>>"),
        ] {
            let at = source.find(&format!("{name} =")).unwrap() + name.len();
            assert!(
                hints.contains(&(at, expected.to_string())),
                "{name}: {hints:?}"
            );
        }
        assert!(
            hints.iter().all(|(_, hint)| !hint.contains('$')),
            "{hints:?}"
        );
    }
}
