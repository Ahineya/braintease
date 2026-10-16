#[cfg(test)]
mod tests {
    use super::super::{Analysis, CompletionKind, SymbolKind};
    use crate::ast::Span;
    const SOURCE: &str = "trait Produce { type Output: Printable; fn produce(&self) -> Self::Output; } struct S { n:i16 } impl Produce for S { type Output=String; fn produce(&self) -> Self::Output { \"hello\" } } fn get<T:Produce>(value:T) -> T::Output { value.produce() } fn main() { let s=S{n:1}; let value:S::Output=get(s); println(value); }";

    #[test]
    fn projections_navigate_to_the_contract_and_have_concrete_signatures() {
        let doc = Analysis::open(None, SOURCE);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
        let def = SOURCE.find("Output").unwrap();
        for spelling in ["Self::Output", "type Output=", "T::Output", "S::Output"] {
            let start = SOURCE.find(spelling).unwrap();
            let at = start + spelling.find("Output").unwrap();
            assert_eq!(doc.definition(at).unwrap().1, Span::new(def, def + 6));
            assert_eq!(doc.token_type(Span::new(at, at + 6)), 2);
        }
        assert!(doc
            .hover(SOURCE.find("S::Output").unwrap() + 3)
            .unwrap()
            .contains("String"));
        assert!(doc
            .symbols
            .iter()
            .any(|s| s.name == "Output" && s.kind == SymbolKind::AssociatedType));
        let edited = SOURCE.replace("let value:S::Output=get(s);", "s.pro;");
        let doc = Analysis::open(None, &edited);
        let items = doc.completions(edited.find("s.pro;").unwrap() + 5);
        let method = items.iter().find(|m| m.label == "produce").unwrap();
        assert_eq!(method.detail, "fn() -> String");
    }

    #[test]
    fn associated_completion_covers_self_bounds_and_qualified_paths() {
        for replacement in ["Ou", ""] {
            for (original, prefix) in [
                ("T::Output", "T"),
                ("S::Output", "S"),
                ("Self::Output", "Self"),
            ] {
                let edited = SOURCE.replacen(original, &format!("{prefix}::{replacement}"), 1);
                let doc = Analysis::open(None, &edited);
                let at = edited.find(&format!("{prefix}::{replacement}")).unwrap()
                    + prefix.len()
                    + 2
                    + replacement.len();
                let items = doc.completions(at);
                let item = items
                    .iter()
                    .find(|i| i.label == "Output")
                    .unwrap_or_else(|| panic!("missing {original}: {:?}", doc.diagnostics));
                assert!(matches!(item.kind, CompletionKind::AssociatedType));
                assert!(item.import_edit.is_none());
            }
        }
        let edited = SOURCE.replace("T::Output", "<T as Produce>::Ou");
        let doc = Analysis::open(None, &edited);
        assert!(doc
            .completions(edited.find("::Ou { value").unwrap() + 4)
            .iter()
            .any(|i| i.label == "Output"));
        let source =
            "trait A { type Output; } trait B { type Output; } fn f<T:A+B>(x:T, out:T::Ou) {}";
        let doc = Analysis::open(None, source);
        assert!(!doc
            .completions(source.find("::Ou").unwrap() + 4)
            .iter()
            .any(|i| i.label == "Output"));
    }

    #[test]
    fn member_types_do_not_leak_into_unqualified_completion() {
        let doc = Analysis::open(None, SOURCE);
        let items = doc.completions(SOURCE.find("println(value)").unwrap());
        assert!(!items.iter().any(|i| i.label == "Output"));
    }

    #[test]
    fn imported_projections_preserve_aliases_and_definition_locations() {
        let root = std::env::temp_dir().join(format!("rfl-associated-ide-{}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        let library = root.join("library.rfl");
        let library_source = "export trait Produce { type Output; fn produce(&self) -> Self::Output; } export struct S { n:i16 } impl Produce for S { type Output=String; fn produce(&self)->Self::Output { \"hi\" } }";
        std::fs::write(&library, library_source).unwrap();
        let path = root.join("main.rfl");
        let source = "import { Produce as Source, S as Item } from \"./library\"; fn f<T:Source>(x:T) -> <T as Source>::Output { x.produce() } fn main() { let value:Item::Output=f(Item{n:1}); }";
        let doc = Analysis::open(Some(&path), source);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
        for at in [
            source.find("::Output { x").unwrap() + 2,
            source.find("Item::Output").unwrap() + 6,
        ] {
            let (file, span) = doc.definition(at).unwrap();
            assert_eq!(file.unwrap(), library.canonicalize().unwrap());
            assert_eq!(span.start, library_source.find("Output").unwrap());
        }
        let edited = source.replace("Item::Output", "Item::Ou");
        let doc = Analysis::open(Some(&path), &edited);
        assert!(doc
            .completions(edited.find("Item::Ou").unwrap() + 8)
            .iter()
            .any(|i| i.label == "Output"));
        let edited = source.replace("<T as Source>::Output", "T::Ou");
        let doc = Analysis::open(Some(&path), &edited);
        assert!(doc
            .completions(edited.find("T::Ou").unwrap() + 5)
            .iter()
            .any(|i| i.label == "Output"));
        std::fs::remove_dir_all(root).unwrap();
    }
}
