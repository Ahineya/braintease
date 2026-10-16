//! Trait completion and editor regressions.
use super::*;

impl Analysis {
    pub(super) fn trait_qualified_completions(
        &self,
        replace: crate::ast::Span,
    ) -> Option<Vec<Completion>> {
        let prefix = self.source.get(..replace.start)?;
        let tokens = lexer::tokenize(prefix).ok()?;
        let mut tokens = tokens
            .iter()
            .rev()
            .filter(|t| !matches!(t.kind, lexer::TokenKind::Eof));
        if !matches!(tokens.next()?.kind, lexer::TokenKind::ColonColon) {
            return None;
        }
        let owner = tokens.next()?;
        if matches!(owner.kind, lexer::TokenKind::Gt | lexer::TokenKind::Shr) {
            let lexer::TokenKind::Ident(contract) = &tokens.next()?.kind else {
                return None;
            };
            if !matches!(tokens.next()?.kind, lexer::TokenKind::As) {
                return None;
            }
            return Some(self.associated_completions(&self.trait_named(contract)?, replace));
        }
        let lexer::TokenKind::Ident(name) = &owner.kind else {
            return None;
        };
        let mut members =
            std::collections::BTreeMap::<String, std::collections::BTreeMap<String, String>>::new();
        for member in self
            .info
            .type_members
            .iter()
            .filter(|m| m.qualifier == *name && m.scope.contains(replace.start))
        {
            members
                .entry(member.name.clone())
                .or_default()
                .insert(member.contract.clone(), member.detail.clone());
        }
        if !members.is_empty() {
            return Some(
                members
                    .into_iter()
                    .filter(|(_, candidates)| candidates.len() == 1)
                    .map(|(name, candidates)| Completion {
                        label: name,
                        kind: CompletionKind::AssociatedType,
                        detail: candidates.into_values().next().unwrap(),
                        replace,
                        import_edit: None,
                    })
                    .collect(),
            );
        }
        let tr = self.trait_named(name)?;
        Some(
            tr.methods
                .iter()
                .map(|method| {
                    let receiver = match method.receiver {
                        Some(crate::ast::Recv::Shared) => "&Self",
                        Some(crate::ast::Recv::Mut) => "&mut Self",
                        _ => "Self",
                    };
                    let sig = check::trait_method_signature(method);
                    let rest = sig.strip_prefix("fn(").unwrap();
                    let comma = if rest.starts_with(')') { "" } else { ", " };
                    Completion {
                        label: method.name.clone(),
                        kind: CompletionKind::Method,
                        detail: format!("fn({receiver}{comma}{rest}"),
                        replace,
                        import_edit: None,
                    }
                })
                .collect(),
        )
    }
    fn trait_named(&self, name: &str) -> Option<crate::ast::Trait> {
        self.ast
            .traits
            .iter()
            .find(|tr| tr.name == name)
            .cloned()
            .or_else(|| {
                let (import, item) = self.ast.imports.iter().find_map(|import| {
                    import
                        .names
                        .iter()
                        .find(|item| item.alias == name)
                        .map(|item| (import, item))
                })?;
                let (file, _) = crate::modules::import_definition(
                    self.path.as_deref()?,
                    &import.path,
                    Some(&item.name),
                )?;
                let source = std::fs::read_to_string(file).ok()?;
                let tokens = lexer::tokenize(&source).ok()?;
                let (ast, _) = parser::parse_for_ide(&source, &tokens);
                ast.traits
                    .into_iter()
                    .find(|tr| tr.name == item.name && tr.exported)
            })
            .or_else(|| {
                if name != "Printable"
                    || self
                        .ast
                        .imports
                        .iter()
                        .any(|import| import.names.iter().any(|item| item.alias == name))
                {
                    return None;
                }
                let source = include_str!("../../std/io.rfl");
                let tokens = lexer::tokenize(source).ok()?;
                let (ast, _) = parser::parse_for_ide(source, &tokens);
                ast.traits.into_iter().find(|tr| tr.name == "Printable")
            })
    }

    fn associated_completions(
        &self,
        tr: &crate::ast::Trait,
        replace: crate::ast::Span,
    ) -> Vec<Completion> {
        tr.associated_types
            .iter()
            .map(|member| Completion {
                label: member.name.clone(),
                kind: CompletionKind::AssociatedType,
                detail: {
                    let bounds: Vec<_> = member
                        .bounds
                        .iter()
                        .map(|(name, _)| name.as_str())
                        .collect();
                    if bounds.is_empty() {
                        format!("type {}", member.name)
                    } else {
                        format!("type {}: {}", member.name, bounds.join(" + "))
                    }
                },
                replace,
                import_edit: None,
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use crate::{
        ast::Span,
        ide::{Analysis, SymbolKind},
    };
    const SOURCE: &str = "trait Read { fn read(&self, n: i16) -> i16; }\nstruct Value { n: i16 }\nimpl Read for Value { fn read(&self, n: i16) -> i16 { self.n + n } }\nfn bounded<T: Read>(v: T) -> i16 { v.read(2) }\nfn main() { let v = Value { n: 3 }; println(v.read(1)); println(Read::read(&v, 2)); }";

    #[test]
    fn contract_navigation_hover_references_and_signature_help() {
        let doc = Analysis::open(None, SOURCE);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
        let def = SOURCE.find("read(").unwrap();
        for at in [
            SOURCE.find("fn read(&self, n: i16) -> i16 { self").unwrap() + 3,
            SOURCE.find("v.read(2)").unwrap() + 2,
            SOURCE.find("v.read(1)").unwrap() + 2,
            SOURCE.find("Read::read").unwrap() + 6,
        ] {
            assert_eq!(doc.definition(at).unwrap().1, Span::new(def, def + 4));
            assert_eq!(doc.hover(at).unwrap(), "fn(i16) -> i16");
            assert_eq!(doc.references(at).len(), 5);
        }
        let owner = SOURCE.find("Read::").unwrap();
        assert_eq!(
            doc.definition(owner).unwrap().1.start,
            SOURCE.find("Read").unwrap()
        );
        assert_eq!(
            doc.signature(SOURCE.find("v.read(1)").unwrap() + 7)
                .unwrap(),
            ("fn(i16) -> i16".into(), 0)
        );
        let qualified = SOURCE.find("&v, 2").unwrap() + 4;
        assert_eq!(
            doc.signature(qualified).unwrap(),
            ("fn(&Value, i16) -> i16".into(), 1)
        );
        assert!(doc
            .symbols
            .iter()
            .any(|s| s.name == "Read" && s.kind == SymbolKind::Trait));
        let trait_span = Span::new(
            SOURCE.find("Read").unwrap(),
            SOURCE.find("Read").unwrap() + 4,
        );
        assert_eq!(doc.token_type(trait_span), 2);
        assert_eq!(doc.token_type(Span::new(def, def + 4)), 1);
    }

    #[test]
    fn completion_respects_bounds_and_does_not_leak_monomorphizations() {
        let source = SOURCE
            .replace("v.read(2)", "v.re;")
            .replace("-> i16 { v.re; }", "{ v.re; }");
        let doc = Analysis::open(None, &source);
        let at = source.find("v.re").unwrap() + 4;
        let items = doc.completions(at);
        let read = items
            .iter()
            .find(|m| m.label == "read")
            .expect("bounded method");
        assert_eq!(read.detail, "fn(i16) -> i16");
        assert!(read.import_edit.is_none());
        assert!(!items
            .iter()
            .any(|m| matches!(m.label.as_str(), "print" | "println")));
        let unbounded = source.replace("T: Read", "T");
        let doc = Analysis::open(None, &unbounded);
        assert!(!doc
            .completions(unbounded.find("v.re").unwrap() + 4)
            .iter()
            .any(|m| m.label == "read"));
    }

    #[test]
    fn concrete_instantiations_do_not_add_methods_to_generic_completion() {
        let source = "trait Read { fn read(&self) -> i16; } impl Read for i16 { fn read(&self) -> i16 { 1 } } fn bounded<T: Read>(v: T) { v; } fn main() { bounded(1); }";
        let source = source.replace("{ v; }", "{ v.re; }");
        let doc = Analysis::open(None, &source);
        let items = doc.completions(source.find("v.re").unwrap() + 4);
        assert!(items.iter().any(|m| m.label == "read"));
        assert!(!items
            .iter()
            .any(|m| m.label == "print" || m.label == "println"));
        let valid = source.replace("v.re;", "v.read();");
        let doc = Analysis::open(None, &valid);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
        assert_eq!(doc.references(valid.find("v.read").unwrap() + 2).len(), 3);
    }

    #[test]
    fn an_inherent_print_method_does_not_satisfy_printable() {
        let source = "struct Value { n: i16 } impl Value { fn print(&self) {} } fn main() { let v = Value { n: 1 }; v.pr; }";
        let doc = Analysis::open(None, source);
        let items = doc.completions(source.find("v.pr").unwrap() + 4);
        assert!(items.iter().any(|m| m.label == "print"));
        assert!(!items.iter().any(|m| m.label == "println"));
    }

    #[test]
    fn conditional_impls_ambiguity_and_inherent_priority() {
        let declarations = "trait Tag { fn tag(&self) -> i16; }\ntype Boxed<T> = Box(T);\nimpl<T: Printable> Tag for Boxed<T> { fn tag(&self) -> i16 { 1 } }\nstruct Hidden { value: i16 }\n";
        for (value, has_tag) in [("1", true), ("Hidden { value: 1 }", false)] {
            let source = format!("{declarations}fn main() {{ let v = Box({value}); v.ta; }}");
            let doc = Analysis::open(None, &source);
            assert_eq!(
                doc.completions(source.find("v.ta").unwrap() + 4)
                    .iter()
                    .any(|m| m.label == "tag"),
                has_tag
            );
        }
        let source = "trait A { fn read(&self) -> i16; } trait B { fn read(&self) -> i16; } impl A for i16 { fn read(&self) -> i16 { 1 } } impl B for i16 { fn read(&self) -> i16 { 2 } } fn main() { let v = 1; v.re; }";
        let doc = Analysis::open(None, source);
        assert!(!doc
            .completions(source.find("v.re").unwrap() + 4)
            .iter()
            .any(|m| m.label == "read"));
        let source = format!("{source} impl i16 {{ fn read(&self) -> i16 {{ 3 }} }}")
            .replace("v.re;", "v.read();");
        let doc = Analysis::open(None, &source);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
        let def = doc
            .definition(source.find("v.read").unwrap() + 2)
            .unwrap()
            .1;
        assert_eq!(def.start, source.rfind("read(&self)").unwrap());
    }

    #[test]
    fn free_calls_and_builtin_methods_never_navigate_to_an_unrelated_contract() {
        let source = "trait Read { fn read(&self) -> i16; fn len(&self) -> i16; } impl Read for i16 { fn read(&self) -> i16 { 1 } fn len(&self) -> i16 { 2 } } fn read() -> i16 { 3 } fn main() { println(read()); println(1.read()); println(\"hello\".len()); }";
        let doc = Analysis::open(None, source);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
        assert_eq!(
            doc.definition(source.find("println(read())").unwrap() + 8)
                .unwrap()
                .1
                .start,
            source.find("read() -> i16").unwrap()
        );
        assert_eq!(
            doc.definition(source.find("1.read()").unwrap() + 2)
                .unwrap()
                .1
                .start,
            source.find("read(&self)").unwrap()
        );
        assert!(doc
            .definition(source.find("\"hello\".len()").unwrap() + 8)
            .is_none());
        let print = "fn main() { println(42); }";
        let doc = Analysis::open(None, print);
        let (path, span) = doc.definition(print.find("println").unwrap()).unwrap();
        let source = std::fs::read_to_string(path.unwrap()).unwrap();
        assert_eq!(&source[span.start..span.end], "println");
    }

    #[test]
    fn chained_calls_keep_their_selected_method_identity() {
        let source = "trait Text { fn text(&self) -> String; } impl Text for i16 { fn text(&self) -> String { \"hi\" } } fn main() { let v = 1; v.text().print(); println(v.text().len()); }";
        let doc = Analysis::open(None, source);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
        let text = source.find("text(&self)").unwrap();
        assert_eq!(
            doc.definition(source.find("v.text()").unwrap() + 2)
                .unwrap()
                .1
                .start,
            text
        );
        let (path, _) = doc
            .definition(source.find(".print()").unwrap() + 1)
            .unwrap();
        assert!(path.unwrap().ends_with("std/io.rfl"));
        assert!(doc.definition(source.find(".len()").unwrap() + 1).is_none());
    }

    #[test]
    fn inherent_completion_keeps_priority_on_a_borrowed_receiver() {
        let source = "trait Read { fn read(&self) -> i16; } struct V { n: i16 } impl Read for V { fn read(&self) -> i16 { 1 } } impl V { fn read(&self) -> String { \"hi\" } } fn main() { let v = V { n: 1 }; let r = &v; r.re; }";
        let doc = Analysis::open(None, source);
        let items = doc.completions(source.find("r.re").unwrap() + 4);
        let methods: Vec<_> = items.iter().filter(|m| m.label == "read").collect();
        assert_eq!(methods.len(), 1);
        assert_eq!(methods[0].detail, "V");
    }

    #[test]
    fn same_named_chained_builtin_has_no_stale_trait_target() {
        let source = "trait Length { fn len(&self) -> String; } impl Length for i16 { fn len(&self) -> String { \"abc\" } } fn main() { let v = 1; println(v.len().len()); }";
        let doc = Analysis::open(None, source);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
        assert_eq!(
            doc.definition(source.find("v.len").unwrap() + 2)
                .unwrap()
                .1
                .start,
            source.find("len(&self)").unwrap()
        );
        assert!(doc
            .definition(source.rfind(".len()").unwrap() + 1)
            .is_none());
    }

    #[test]
    fn implicit_printable_completes_as_a_trait_and_a_qualifier() {
        let source = "fn main() { let v = 1; Printable::pr(&v); }";
        let doc = Analysis::open(None, source);
        let items = doc.completions(source.find("Printable").unwrap() + 3);
        let printable = items.iter().find(|item| item.label == "Printable").unwrap();
        assert!(matches!(
            printable.kind,
            super::super::CompletionKind::Trait
        ));
        assert!(printable.import_edit.is_none());
        let items = doc.completions(source.find("::pr").unwrap() + 4);
        assert_eq!(items.len(), 1);
        assert_eq!(items[0].label, "print");
        assert_eq!(items[0].detail, "fn(&Self) -> unit");
    }

    #[test]
    fn prelude_print_navigation_is_a_real_library_location() {
        let source = "fn main() { let v = 42; v.print(); }";
        let doc = Analysis::open(None, source);
        let (path, span) = doc.definition(source.find("v.print").unwrap() + 2).unwrap();
        let library = std::fs::read_to_string(path.unwrap()).unwrap();
        assert_eq!(&library[span.start..span.end], "print");
        let source = source.replace("v.print()", "v.pr");
        let doc = Analysis::open(None, &source);
        for name in ["print", "println"] {
            let items = doc.completions(source.find("v.pr").unwrap() + 4);
            let item = items.iter().find(|m| m.label == name).unwrap();
            assert!(item.import_edit.is_none());
        }
    }
}
