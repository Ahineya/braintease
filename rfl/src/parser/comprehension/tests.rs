use super::*;

#[test]
fn disabled_comprehensions_do_not_create_dependencies_and_expansions_keep_body_spans() {
    let source = "#[cfg(target = \"native\")] fn other(){for{n<-Some(1);yield n}} fn main(){}";
    let tokens = crate::lexer::tokenize(source).unwrap();
    assert!(parse(source, &tokens).unwrap().imports.is_empty());
    let native = parse_for_target(source, &tokens, rcc_ir::target::Target::Native).unwrap();
    assert_eq!(native.imports.len(), 1);
    let source = "fn main(){for{n<-Some(1);yield n+1}}";
    let tokens = crate::lexer::tokenize(source).unwrap();
    let ast = parse(source, &tokens).unwrap();
    let ExprKind::Block(block) = &ast.functions[0].body.tail.as_ref().unwrap().kind else {
        panic!()
    };
    let ExprKind::Call { args, .. } = &block.tail.as_ref().unwrap().kind else {
        panic!()
    };
    let ExprKind::Lambda { body, .. } = &args[1].kind else {
        panic!()
    };
    assert!(args[1].span.start <= body.span.start && args[1].span.end >= body.span.end);
    assert!(body.span.start >= args[0].span.end);
}

#[test]
fn auto_import_in_a_comprehension_never_edits_a_compiler_import() {
    let path =
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("examples/comprehension-import-test.rfl");
    let source = "fn main(){let p=for{n<-Some(1);yield map(Some(n),fn(x:i16)=>x+1)};}";
    let analysis = crate::ide::Analysis::open(Some(&path), source);
    let at = source.find("map(").unwrap();
    let fixes = analysis.import_fixes(at, at);
    let fix = fixes
        .iter()
        .find(|fix| fix.module == "std/fp")
        .unwrap_or_else(|| panic!("{:?}", analysis.diagnostics));
    assert_eq!(fix.edit_span, Span::empty());
    assert!(
        fix.new_text.starts_with("import { map }"),
        "{}",
        fix.new_text
    );
    let mut edited = source.to_owned();
    edited.replace_range(fix.edit_span.start..fix.edit_span.end, &fix.new_text);
    let after = crate::ide::Analysis::open(Some(&path), &edited);
    assert!(after.diagnostics.is_empty(), "{:?}", after.diagnostics);
}

use std::path::PathBuf;
