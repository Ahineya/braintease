//! Outer documentation comments follow declaration identities through navigation.
use std::collections::HashMap;

use super::Analysis;
use crate::ast::{Module, Span};
use crate::lexer::{self, Token, TokenKind as K};

#[derive(Clone)]
pub(super) struct DocComment {
    signature: String,
    markdown: String,
}

impl Analysis {
    pub fn hover_markdown(&self, offset: usize) -> Option<String> {
        let docs = self.documentation_at(offset);
        let signature = self
            .hover(offset)
            .or_else(|| docs.as_ref().map(|d| d.signature.clone()))?;
        let mut markdown = format!("```rfl\n{signature}\n```");
        if let Some(docs) = docs {
            markdown.push_str("\n\n");
            markdown.push_str(&docs.markdown);
        }
        Some(markdown)
    }

    fn documentation_at(&self, offset: usize) -> Option<DocComment> {
        let ident = super::ident_around(&self.source, offset);
        if let Some(docs) = self.documentation.get(&ident.start) {
            return Some(docs.clone());
        }
        // Definition lookup also handles import aliases and resolved method calls.
        if let Some((path, span)) = self.definition(offset) {
            let local = path.is_none() || path == self.path;
            if local {
                return self.documentation.get(&span.start).cloned();
            }
            let source = std::fs::read_to_string(path?).ok()?;
            let tokens = lexer::tokenize(&source).ok()?;
            let (ast, _) = crate::parser::parse_for_ide(&source, &tokens);
            return collect(&source, &ast).remove(&span.start);
        }
        None
    }
}

pub(super) fn collect(source: &str, module: &Module) -> HashMap<usize, DocComment> {
    let Ok(tokens) = lexer::tokenize_with_trivia(source) else {
        return HashMap::new();
    };
    let mut result = HashMap::new();
    let mut add = |span: Span, signature: String, constructor: bool| {
        let i = tokens.partition_point(|token| token.span.start < span.start);
        if let Some(markdown) = before(source, &tokens, i, constructor) {
            result.insert(
                span.start,
                DocComment {
                    signature,
                    markdown,
                },
            );
        }
    };
    for f in &module.functions {
        add(f.name_span, header(source, &tokens, f.name_span), false);
    }
    for f in &module.externs {
        add(f.name_span, header(source, &tokens, f.name_span), false);
    }
    for item in &module.structs {
        add(item.name_span, format!("struct {}", item.name), false);
        for field in &item.fields {
            add(
                field.name_span,
                format!(
                    "{}: {}",
                    field.name,
                    crate::check::completion_type_name(&field.ty)
                ),
                false,
            );
        }
    }
    for item in &module.adts {
        add(item.name_span, format!("type {}", item.name), false);
        for ctor in &item.ctors {
            add(ctor.name_span, ctor.name.clone(), true);
        }
    }
    for item in &module.consts {
        add(
            item.name_span,
            format!(
                "const {}: {}",
                item.name,
                crate::check::completion_type_name(&item.ty)
            ),
            false,
        );
    }
    for item in &module.traits {
        add(item.name_span, format!("trait {}", item.name), false);
        for method in &item.methods {
            add(
                method.name_span,
                header(source, &tokens, method.name_span),
                false,
            );
        }
        for associated in &item.associated_types {
            add(
                associated.name_span,
                format!("type {}", associated.name),
                false,
            );
        }
    }
    for item in &module.impls {
        for method in &item.items {
            add(
                method.name_span,
                header(source, &tokens, method.name_span),
                false,
            );
        }
        for associated in &item.associated_types {
            add(
                associated.name_span,
                format!("type {}", associated.name),
                false,
            );
        }
    }
    result
}

fn before(source: &str, tokens: &[Token], mut i: usize, constructor: bool) -> Option<String> {
    // Step over the declaration keyword and its modifiers, then attributes.
    if i > 0
        && matches!(
            tokens[i - 1].kind,
            K::Fn | K::Struct | K::Trait | K::Type | K::Const
        )
    {
        i -= 1;
    }
    if constructor && i > 0 && tokens[i - 1].kind == K::Pipe {
        i -= 1;
    }
    let mut lines = Vec::new();
    while i > 0 {
        let token = &tokens[i - 1];
        match token.kind {
            K::Export | K::Async | K::Extern | K::Str(_) | K::BlockComment => i -= 1,
            K::LineComment => {
                let text = source.get(token.span.start..token.span.end)?;
                if let Some(text) = text
                    .strip_prefix("///")
                    .filter(|text| !text.starts_with('/'))
                {
                    // A trailing comment belongs to the preceding line.
                    let line_start = source[..token.span.start]
                        .rfind('\n')
                        .map_or(0, |at| at + 1);
                    if !source[line_start..token.span.start].trim().is_empty() {
                        break;
                    }
                    let text = text.strip_suffix('\r').unwrap_or(text);
                    lines.push(text.strip_prefix(' ').unwrap_or(text));
                }
                i -= 1;
            }
            K::RBracket => {
                let mut start = i - 1;
                let mut depth = 1;
                while start > 0 && depth > 0 {
                    start -= 1;
                    match tokens[start].kind {
                        K::RBracket => depth += 1,
                        K::LBracket => depth -= 1,
                        _ => {}
                    }
                }
                if depth != 0 || start == 0 || tokens[start - 1].kind != K::Hash {
                    break;
                }
                i = start - 1;
            }
            _ => break,
        }
    }
    lines.reverse();
    let text = lines.join("\n").trim_matches('\n').to_string();
    (!text.is_empty()).then_some(text)
}

fn header(source: &str, tokens: &[Token], span: Span) -> String {
    let i = tokens.partition_point(|token| token.span.start < span.start);
    let start = tokens[..i]
        .iter()
        .rposition(|token| token.kind == K::Fn)
        .unwrap_or(i);
    let mut depth = 0usize;
    let mut end = span.end;
    for token in &tokens[i..] {
        match token.kind {
            K::LParen | K::LBracket => depth += 1,
            K::RParen | K::RBracket => depth = depth.saturating_sub(1),
            K::LBrace | K::Semi if depth == 0 => break,
            K::Eof => break,
            _ => {}
        }
        end = token.span.end;
    }
    source[tokens[start].span.start..end].trim().to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn markdown_and_attributes_survive_unsaved_source_and_crlf() {
        let source = "/// Read a **value**. 🙂\r\n///\r\n/// # Example\r\n/// ```rfl\r\n/// let n = value();\r\n/// ```\r\n#[cfg(target = \"ripple\")]\r\nexport fn value()->i16{42}\r\nfn main(){value();}\r\n";
        let analysis = Analysis::open(None, source);
        assert!(
            analysis.diagnostics.is_empty(),
            "{:?}",
            analysis.diagnostics
        );
        for at in [
            source.find("fn value").unwrap() + 3,
            source.rfind("value()").unwrap(),
        ] {
            let hover = analysis.hover_markdown(at).unwrap();
            assert!(hover.starts_with("```rfl\n"), "{hover}");
            assert!(
                hover.contains("Read a **value**. 🙂\n\n# Example\n```rfl\nlet n = value();\n```"),
                "{hover}"
            );
            assert!(!hover.contains('\r'));
        }
        let changed = source.replace("Read a **value**", "Updated documentation");
        let analysis = Analysis::open(None, &changed);
        assert!(analysis
            .hover_markdown(changed.rfind("value()").unwrap())
            .unwrap()
            .contains("Updated documentation"));
        let formatted = crate::format_source(source).unwrap();
        let analysis = Analysis::open(None, &formatted);
        assert!(analysis
            .hover_markdown(formatted.rfind("value()").unwrap())
            .unwrap()
            .contains("Read a **value**. 🙂\n\n# Example"));
    }

    #[test]
    fn documentation_belongs_to_declarations_not_similarly_named_variables() {
        let source = "/// The documented function.\nfn value()->i16{1}\n// Ordinary.\nfn ordinary(){}\n//// Not a doc comment.\nfn four_slashes(){}\nfn main(){let value=1;println(value);let s=\"/// not documentation\";}";
        let analysis = Analysis::open(None, source);
        for text in ["ordinary()", "four_slashes()", "println(value)"] {
            let at = source.find(text).unwrap() + if text.starts_with("println") { 8 } else { 0 };
            assert!(!analysis
                .hover_markdown(at)
                .unwrap_or_default()
                .contains("The documented function"));
        }
        assert!(!analysis
            .hover_markdown(source.find("ordinary()").unwrap())
            .unwrap_or_default()
            .contains("Ordinary"));
        assert!(!analysis
            .hover_markdown(source.find("four_slashes()").unwrap())
            .unwrap_or_default()
            .contains("Not a doc"));
    }

    #[test]
    fn ordinary_comments_and_attributes_do_not_interrupt_documentation() {
        let source = "/// First paragraph.\n#[cfg(target = \"ripple\")]\n// A regular comment.\n/* A block comment. */\n///\n/// Second paragraph.\nexport fn f(){}";
        let analysis = Analysis::open(None, source);
        let hover = analysis
            .hover_markdown(source.find("f()").unwrap())
            .unwrap();
        assert!(
            hover.ends_with("First paragraph.\n\nSecond paragraph."),
            "{hover}"
        );
        assert!(!hover.contains("regular comment"));
        assert!(!hover.contains("block comment"));
    }

    #[test]
    fn implementation_declarations_keep_their_own_docs_and_calls_show_the_contract() {
        let source = "trait Read {\n/// The read contract.\nfn read(&self)->i16; }\nstruct Value{n:i16}\nimpl Read for Value {\n/// This implementation reads n.\nfn read(&self)->i16{self.n}}\nfn main(){let v=Value{n:1};v.read();}";
        let analysis = Analysis::open(None, source);
        assert!(
            analysis.diagnostics.is_empty(),
            "{:?}",
            analysis.diagnostics
        );
        assert!(analysis
            .hover_markdown(source.rfind("fn read").unwrap() + 3)
            .unwrap()
            .contains("This implementation reads n."));
        assert!(analysis
            .hover_markdown(source.rfind("read();").unwrap())
            .unwrap()
            .contains("The read contract."));
    }

    #[test]
    fn types_fields_constructors_and_associated_types_have_documentation() {
        let source = "/// A point.\nstruct Point{\n/// Horizontal coordinate.\nx:i16}\n/// A choice.\ntype Choice =\n/// An empty choice.\n| Empty;\n/// A reader.\ntrait Read{\n/// The result type.\ntype Output;\n/// Reads once.\nfn read(&self)->Self::Output;}\nimpl Point{\n/// Returns the horizontal coordinate.\nexport fn get(&self)->i16{self.x}}\nfn main(){let p=Point{x:1};p.get();}";
        let analysis = Analysis::open(None, source);
        assert!(
            analysis.diagnostics.is_empty(),
            "{:?}",
            analysis.diagnostics
        );
        for (needle, doc) in [
            ("Point{", "A point."),
            ("x:i16", "Horizontal coordinate."),
            ("Choice =", "A choice."),
            ("Empty;", "An empty choice."),
            ("Read{", "A reader."),
            ("Output;", "The result type."),
            ("read(&", "Reads once."),
            ("get();", "Returns the horizontal coordinate."),
            ("x}}", "Horizontal coordinate."),
        ] {
            let hover = analysis
                .hover_markdown(source.find(needle).unwrap())
                .unwrap();
            assert!(hover.contains(doc), "{needle}: {hover}");
        }
    }

    #[test]
    fn imported_aliases_and_methods_use_their_own_definitions() {
        let root = std::env::temp_dir().join(format!("rfl-documentation-{}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        for module in ["left", "right"] {
            std::fs::write(root.join(format!("{module}.rfl")), format!("/// {module} value.\nexport struct Value{{\n/// The {module} field.\nn:i16}}\n/// The {module} constant.\nexport const VALUE:i16=1;\n/// Makes a {module} value.\nexport fn make()->Value{{Value{{n:1}}}}\nimpl Value{{\n/// Reads the {module} value.\nexport fn read(&self)->i16{{self.n}}}}\n")).unwrap();
        }
        let source = "import {Value as Left,make as make_left,VALUE as LEFT} from \"./left\";import {Value as Right,make as make_right,VALUE as RIGHT} from \"./right\";fn main(){let left=make_left();let right=make_right();left.read();right.read();left.n;right.n;LEFT;RIGHT;}";
        let analysis = Analysis::open(Some(&root.join("main.rfl")), source);
        assert!(
            analysis.diagnostics.is_empty(),
            "{:?}",
            analysis.diagnostics
        );
        for (needle, delta, expected) in [
            ("Left,", 0, "left value."),
            ("make_left()", 0, "Makes a left value."),
            ("make_right()", 0, "Makes a right value."),
            ("left.read()", 5, "Reads the left value."),
            ("right.read()", 6, "Reads the right value."),
            ("left.n", 5, "The left field."),
            ("right.n", 6, "The right field."),
            ("LEFT;", 0, "The left constant."),
            ("RIGHT;", 0, "The right constant."),
        ] {
            let hover = analysis
                .hover_markdown(source.find(needle).unwrap() + delta)
                .unwrap();
            assert!(
                hover.contains(expected),
                "{needle}: {hover}, definition: {:?}",
                analysis.definition(source.find(needle).unwrap() + delta)
            );
            assert!(!hover.contains('$'), "{hover}");
        }
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn standard_library_public_api_has_documentation() {
        let directory = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("std");
        for entry in std::fs::read_dir(directory).unwrap() {
            let path = entry.unwrap().path();
            if path.extension().is_none_or(|ext| ext != "rfl") {
                continue;
            }
            let source = std::fs::read_to_string(&path).unwrap();
            let tokens = lexer::tokenize(&source).unwrap();
            for target in [
                rcc_ir::target::Target::Ripple,
                rcc_ir::target::Target::Native,
            ] {
                let ast = crate::parser::parse_for_target(&source, &tokens, target).unwrap();
                let docs = collect(&source, &ast);
                let mut required = Vec::new();
                required.extend(
                    ast.functions
                        .iter()
                        .filter(|f| f.exported)
                        .map(|f| f.name_span),
                );
                required.extend(
                    ast.consts
                        .iter()
                        .filter(|c| c.exported)
                        .map(|c| c.name_span),
                );
                required.extend(ast.adts.iter().filter(|a| a.exported).map(|a| a.name_span));
                for item in ast.structs.iter().filter(|s| s.exported) {
                    required.push(item.name_span);
                    required.extend(item.fields.iter().map(|field| field.name_span));
                }
                for item in ast.traits.iter().filter(|t| t.exported) {
                    required.push(item.name_span);
                    required.extend(item.methods.iter().map(|method| method.name_span));
                    required.extend(item.associated_types.iter().map(|ty| ty.name_span));
                }
                for item in &ast.impls {
                    for method in &item.items {
                        let line = source[..method.name_span.start]
                            .rsplit('\n')
                            .next()
                            .unwrap();
                        if line.contains("export fn") {
                            required.push(method.name_span);
                        }
                    }
                }
                for span in required {
                    assert!(
                        docs.contains_key(&span.start),
                        "{}: missing docs for {}",
                        path.display(),
                        &source[span.start..span.end]
                    );
                }
            }
        }
    }
}
