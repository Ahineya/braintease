//! Small source edits for unresolved standard-library names. Export visibility
//! comes from the module resolver, and unresolved names from the type checker.

use crate::ast::{Import, Module, Span};
use crate::error::CompileError;
use crate::lexer::{self, Token, TokenKind};

pub(crate) struct ImportFix {
    pub name: String,
    pub module: String,
    pub diagnostic: String,
    pub symbol_span: Span,
    pub edit_span: Span,
    pub new_text: String,
}

pub(crate) fn fixes(
    source: &str,
    ast: &Module,
    diagnostics: &[CompileError],
    start: usize,
    end: usize,
) -> Vec<ImportFix> {
    let Ok(tokens) = lexer::tokenize_with_trivia(source) else {
        return Vec::new();
    };
    let mut fixes = Vec::new();
    for diagnostic in diagnostics {
        let name = [
            "unknown name `",
            "unknown function `",
            "unknown type `",
            "unknown constructor `",
            "unknown constant `",
            "unknown struct `",
        ]
        .iter()
        .find_map(|prefix| diagnostic.detail.strip_prefix(prefix)?.strip_suffix('`'));
        let (Some(name), Some(offset)) = (name, diagnostic.offset) else {
            continue;
        };
        let Some(token) = tokens.iter().find(|token| {
            token.span.contains(offset)
                && matches!(&token.kind, TokenKind::Ident(word) if word == name)
        }) else {
            continue;
        };
        let span = token.span;
        let selected = if start == end {
            span.start <= start && start <= span.end
        } else {
            span.start < end && start < span.end
        };
        if !selected
            || ast
                .imports
                .iter()
                .any(|import| import.names.iter().any(|item| item.alias == name))
        {
            continue;
        }
        for module in crate::modules::std_import_candidates(name) {
            let (edit_span, new_text) = import_edit(source, &tokens, &ast.imports, &module, name);
            if fixes
                .iter()
                .any(|fix: &ImportFix| fix.name == name && fix.module == module)
            {
                continue;
            }
            fixes.push(ImportFix {
                name: name.to_string(),
                module,
                diagnostic: diagnostic.detail.clone(),
                symbol_span: span,
                edit_span,
                new_text,
            });
        }
    }
    fixes
}

pub(crate) fn completion_edit(
    source: &str,
    ast: &Module,
    module: &str,
    name: &str,
) -> Option<(Span, String)> {
    let tokens = lexer::tokenize_with_trivia(source).ok()?;
    Some(import_edit(source, &tokens, &ast.imports, module, name))
}

fn import_edit(
    source: &str,
    tokens: &[Token],
    imports: &[Import],
    module: &str,
    name: &str,
) -> (Span, String) {
    let newline = if source.contains("\r\n") {
        "\r\n"
    } else {
        "\n"
    };
    if let Some(import) = imports.iter().find(|import| {
        !import.is_implicit() && import.path.strip_suffix(".rfl").unwrap_or(&import.path) == module
    }) {
        if let Some(first) = import.names.first() {
            let before = &source[import.span.start..first.span.start];
            let addition = if before.contains('\n') {
                let line_start = source[..first.span.start]
                    .rfind('\n')
                    .map_or(0, |at| at + 1);
                let indent: String = source[line_start..first.span.start]
                    .chars()
                    .take_while(|ch| matches!(ch, ' ' | '\t'))
                    .collect();
                format!("{name},{newline}{indent}")
            } else {
                format!("{name}, ")
            };
            return (Span::new(first.span.start, first.span.start), addition);
        }
    }
    // Keep leading header comments and all existing import aliases/comments.
    let first = tokens.iter().find(|token| {
        !matches!(
            token.kind,
            TokenKind::LineComment | TokenKind::BlockComment | TokenKind::Eof
        )
    });
    let at = first.map_or(source.len(), |token| token.span.start);
    let separator = if first.is_some_and(|token| matches!(token.kind, TokenKind::Import)) {
        ""
    } else {
        newline
    };
    (
        Span::new(at, at),
        format!("import {{ {name} }} from \"{module}\";{newline}{separator}"),
    )
}

#[cfg(test)]
mod tests {
    use crate::ide::Analysis;

    fn apply(source: &str, name: &str, module: &str) -> String {
        let path =
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/auto-import-test.rfl");
        let analysis = Analysis::open(Some(&path), source);
        let at = source.rfind(name).unwrap();
        let fixes = analysis.import_fixes(at, at);
        let fix = fixes
            .iter()
            .find(|fix| fix.module == module)
            .unwrap_or_else(|| panic!("no {module} import for {name}: {:?}", analysis.diagnostics));
        let mut edited = source.to_string();
        edited.replace_range(fix.edit_span.start..fix.edit_span.end, &fix.new_text);
        let after = Analysis::open(Some(&path), &edited);
        assert!(
            after.diagnostics.is_empty(),
            "{edited}\n{:?}",
            after.diagnostics
        );
        assert!(after.import_fixes(0, edited.len()).is_empty());
        edited
    }

    #[test]
    fn imports_functions_constants_and_types() {
        assert_eq!(
            apply("fn value() -> f64 { sin(0.0) }\n", "sin", "std/math"),
            "import { sin } from \"std/math\";\n\nfn value() -> f64 { sin(0.0) }\n"
        );
        apply("fn value() -> f64 { PI }\n", "PI", "std/math");
        apply("fn value(xs: List<i16>) -> i16 { 0 }\n", "List", "std/list");
    }

    #[test]
    fn merges_imports_preserving_aliases_comments_and_line_endings() {
        let source = "// header 🦀\r\nimport { cos as cosine } from \"std/math.rfl\"; // keep\r\nfn value() -> f64 { sin(0.0) }\r\n";
        let edited = apply(source, "sin", "std/math");
        assert!(edited.contains("import { sin, cos as cosine }"));
        assert!(edited.starts_with("// header 🦀\r\n"));
        assert!(edited.contains("; // keep\r\n"));
        let source = "import {\n    // leave this comment\n    cos as cosine,\n} from \"std/math\";\nfn value() -> f64 { sin(0.0) }\n";
        assert!(apply(source, "sin", "std/math").contains("    sin,\n    cos as cosine,"));
    }

    #[test]
    fn alias_does_not_prevent_importing_the_original_name() {
        apply(
            "import { sin as sine } from \"std/math\";\nfn value() -> f64 { sin(0.0) }",
            "sin",
            "std/math",
        );
    }

    #[test]
    fn uses_export_rules_and_keeps_ambiguous_choices() {
        assert_eq!(
            crate::modules::std_import_candidates("sin"),
            vec!["std/math"]
        );
        assert_eq!(
            crate::modules::std_import_candidates("is_ascii"),
            vec!["std/char", "std/string"]
        );
        assert_eq!(
            crate::modules::std_import_candidates("Cons"),
            vec!["std/list"]
        );
        assert!(crate::modules::std_import_candidates("region_eq").is_empty());
        assert!(crate::modules::std_import_candidates("not_a_std_symbol").is_empty());
        let source = "fn value() -> bool { is_ascii('a') }";
        let analysis = Analysis::open(None, source);
        assert_eq!(
            analysis
                .import_fixes(
                    source.find("is_ascii").unwrap(),
                    source.find("is_ascii").unwrap()
                )
                .len(),
            2
        );
    }

    #[test]
    fn does_not_offer_imports_for_resolved_names_or_unrelated_ranges() {
        for source in [
            "fn sin(x: f64) -> f64 { x } fn value() -> f64 { sin(0.0) }",
            "fn value() -> f64 { let sin = 1.0; sin }",
            "fn value() { println(1); }",
            "// sin\nfn value() {}",
            "fn value() { let s = \"sin\"; }",
        ] {
            let analysis = Analysis::open(None, source);
            assert!(
                analysis.import_fixes(0, source.len()).is_empty(),
                "{source}"
            );
        }
        let source = "fn value() -> f64 { sin(0.0) }";
        assert!(Analysis::open(None, source).import_fixes(0, 0).is_empty());
    }
}
