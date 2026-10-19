use crate::{ide::Analysis, rename::Workspace};
use std::{
    collections::BTreeMap,
    path::PathBuf,
    sync::atomic::{AtomicUsize, Ordering},
};
static SERIAL: AtomicUsize = AtomicUsize::new(0);
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "rfl-macro-editor-{}-{}",
            std::process::id(),
            SERIAL.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&path).unwrap();
        std::fs::write(path.join("macro.rfl"),r#"import {TokenStream,splice} from "std/macro";#[proc_macro]export fn keep(input:TokenStream)->TokenStream{splice("{let temporary=$0;temporary}",[input])}#[proc_macro]export fn declare(input:TokenStream)->TokenStream{splice("export fn $0()->i16{42}",[input])}"#).unwrap();
        Self(std::fs::canonicalize(path).unwrap())
    }
    fn main(&self, source: &str) -> PathBuf {
        let path = self.0.join("main.rfl");
        std::fs::write(&path, source).unwrap();
        path
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
#[test]
fn checked_facts_navigate_and_rename_through_splices() {
    let f = Fixture::new();
    let source="import macro {keep,declare} from \"./macro.rfl\";\ndeclare!{answer};\nfn main(){let seed=20;println(keep!{seed+22});println(answer());}";
    let path = f.main(source);
    let doc = Analysis::open(Some(&path), source);
    assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
    let at = source.find("seed+22").unwrap();
    assert!(doc.hover(at).unwrap().contains("i16"));
    assert_eq!(
        doc.definition(at).unwrap().1.start,
        source.find("seed=20").unwrap()
    );
    assert_eq!(
        doc.definition(source.rfind("answer()").unwrap())
            .unwrap()
            .1
            .start,
        source.find("answer}").unwrap()
    );
    assert!(doc.completions(at).iter().any(|c| c.label == "seed"));
    assert!(doc
        .completions(at)
        .iter()
        .all(|c| !c.label.contains('$') && c.label != "temporary"));
    let workspace = Workspace::load(&[f.0.clone()], BTreeMap::new());
    let edits = workspace.rename(&path, at, "number").unwrap();
    assert_eq!(edits.len(), 2);
    assert!(edits
        .iter()
        .all(|e| &source[e.span.start..e.span.end] == "seed"));
    let edits = workspace
        .rename(&path, source.rfind("answer()").unwrap(), "value")
        .unwrap();
    assert_eq!(edits.len(), 2);
    let edits = workspace
        .rename(&path, source.find("keep!").unwrap(), "retain")
        .unwrap();
    assert_eq!(edits.len(), 3);
}
#[test]
fn incomplete_macro_discards_the_damaged_declaration_and_checks_the_rest() {
    let source = "fn broken(){ keep! { raw text isn't complete\nfn other(){missing;}";
    let doc = Analysis::open(None, source);
    assert!(
        doc.diagnostics
            .iter()
            .any(|e| e.detail.contains("unclosed macro")),
        "{:?}",
        doc.diagnostics
    );
    assert!(
        doc.diagnostics.iter().any(|e| e.detail.contains("missing")),
        "{:?}",
        doc.diagnostics
    );
}

#[test]
fn compiled_artifact_is_reused_but_transitive_source_changes_rebuild_real_code() {
    use super::executor::{Executor, NativeExecutor};
    let f = Fixture::new();
    let package = f.0.join("macro.rfl");
    std::fs::write(&package,r#"import {TokenStream,quote} from "std/macro";import {meaning} from "./dependency.rfl";#[proc_macro]export fn value(input:TokenStream)->TokenStream{quote(meaning())}"#).unwrap();
    std::fs::write(
        f.0.join("dependency.rfl"),
        "export fn meaning()->String{\"42\"}",
    )
    .unwrap();
    let first = super::executor::executable(&package, "value").unwrap();
    let modified = std::fs::metadata(&first).unwrap().modified().unwrap();
    let second = super::executor::executable(&package, "value").unwrap();
    assert_eq!(first, second);
    assert_eq!(
        modified,
        std::fs::metadata(second).unwrap().modified().unwrap()
    );
    let mut executor = NativeExecutor;
    assert_eq!(
        executor
            .execute(&package, "value", crate::Target::Native, &[])
            .unwrap()[0]
            .text,
        "42"
    );
    std::fs::write(
        f.0.join("dependency.rfl"),
        "export fn meaning()->String{\"43\"}",
    )
    .unwrap();
    let rebuilt = super::executor::executable(&package, "value").unwrap();
    assert_ne!(first, rebuilt);
    assert_eq!(
        executor
            .execute(&package, "value", crate::Target::Ripple, &[])
            .unwrap()[0]
            .text,
        "43"
    );
}

#[test]
fn imported_generated_declarations_navigate_to_the_dependency_source() {
    let f = Fixture::new();
    let generated = "import macro {declare} from \"./macro.rfl\";declare!{provided};";
    std::fs::write(f.0.join("generated.rfl"), generated).unwrap();
    let source =
        "import {provided as answer} from \"./generated.rfl\";fn main(){println(answer());}";
    let path = f.main(source);
    let doc = Analysis::open(Some(&path), source);
    assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
    for offset in [
        source.find("provided").unwrap(),
        source.find("as answer").unwrap() + 3,
        source.find("answer()").unwrap(),
    ] {
        let (file, span) = doc.definition(offset).unwrap();
        assert_eq!(file, Some(f.0.join("generated.rfl")));
        assert_eq!(span.start, generated.find("provided}").unwrap());
    }
}

#[test]
fn an_incomplete_group_that_consumed_the_function_close_recovers_at_the_next_item() {
    let source = "fn broken(){ keep!{raw text}\nfn other(){missing;}";
    let doc = Analysis::open(None, source);
    assert!(
        doc.diagnostics.iter().any(|e| e.detail.contains("missing")),
        "{:?}",
        doc.diagnostics
    );
}

#[test]
fn unsaved_macro_modules_compile_and_navigate_without_being_written_to_disk() {
    let f = Fixture::new();
    let implementation = std::fs::read_to_string(f.0.join("macro.rfl")).unwrap();
    let unsaved = f.0.join("unsaved.rfl");
    let _overlay = super::source::overlay(BTreeMap::from([(unsaved.clone(), implementation)]));
    let source = "import macro {keep} from \"./unsaved.rfl\";fn main(){println(keep!{42});}";
    let path = f.main(source);
    let doc = Analysis::open(Some(&path), source);
    assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
    assert_eq!(
        doc.definition(source.find("keep!").unwrap()).unwrap().0,
        Some(unsaved.clone())
    );
    assert!(!unsaved.exists());
}
#[test]
fn recovered_buffers_still_invalidate_when_a_macro_dependency_changes() {
    let f = Fixture::new();
    let source="import macro {keep} from \"./macro.rfl\";\nfn okay()->i16{keep!{42}}\nfn broken(){keep!{unfinished\nfn other(){missing;}";
    let path = f.main(source);
    let before = super::editor::fingerprint(Some(&path), source);
    let implementation = std::fs::read_to_string(f.0.join("macro.rfl")).unwrap();
    std::fs::write(
        f.0.join("macro.rfl"),
        format!("{implementation}\n// changed\n"),
    )
    .unwrap();
    assert_ne!(before, super::editor::fingerprint(Some(&path), source));
}
