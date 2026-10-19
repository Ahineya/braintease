//! End-to-end acceptance gates for user-authored procedural macros.
#[path = "support/fp.rs"]
mod support;

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
static SERIAL: AtomicUsize = AtomicUsize::new(0);
struct Fixture(PathBuf);
impl Fixture {
    fn new(files: &[(&str, &str)]) -> Self {
        let root = std::env::temp_dir().join(format!(
            "rfl-macro-{}-{}",
            std::process::id(),
            SERIAL.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&root).unwrap();
        for (name, source) in files {
            std::fs::write(root.join(name), source).unwrap();
        }
        Self(root)
    }
    fn main(&self) -> &Path {
        &self.0
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
const IDENTITY: &str = r#"
import {TokenStream} from "std/macro";
#[proc_macro]
export fn identity(input: TokenStream) -> TokenStream { input }
"#;

#[test]
fn large_valid_frames_fit_the_existing_macro_execution_budget() {
    // Keep both directions below the 60,000-byte protocol limit. A long input
    // and many output tokens exercise request reading and response assembly
    // separately; neither should spend its runtime budget recopying a prefix.
    let large_input = format!(
        "import macro {{identity}} from \"./identity.rfl\";fn main(){{println(identity!{{/*{}*/42}});}}",
        "x".repeat(57_000)
    );
    let output_macro = r#"
import {Token,TokenStream,concat,quote} from "std/macro";
#[proc_macro]
export fn identity(input:TokenStream)->TokenStream {
    let mut trivia:Vec<Token> = [];
    for (n in 0..1000) {
        trivia.push(Token{kind:4,text:"            ",origin:"call",syntax:"definition"});
    }
    concat(TokenStream{tokens:trivia,target:"",dependencies:[]},quote("42"))
}
"#;
    for (implementation, main) in [
        (IDENTITY, large_input.as_str()),
        (
            output_macro,
            r#"import macro {identity} from "./identity.rfl";fn main(){println(identity!{});}"#,
        ),
    ] {
        let f = Fixture::new(&[("identity.rfl", implementation), ("main.rfl", main)]);
        let path = f.main().join("main.rfl");
        assert_eq!(rfl::run_native_file(&path, false).unwrap().stdout, "42\n");
        assert_eq!(rfl::run_file(&path).unwrap().stdout, "42\n");
    }
}

#[test]
fn imported_user_macro_keeps_caller_bindings_and_all_group_delimiters() {
    let f = Fixture::new(&[
        ("identity.rfl", IDENTITY),
        (
            "main.rfl",
            r#"
import macro {identity as keep} from "./identity.rfl";
fn keep(n:i16)->i16 { n+1 }
fn main() {
    let n=20;
    println(keep! {n+22});
    println(keep!(keep(40)));
    println(keep![keep!{n}]);
}
"#,
        ),
    ]);
    let output = rfl::run_native_file(&f.main().join("main.rfl"), false).unwrap();
    assert_eq!(output.status, 0);
    assert_eq!(output.stdout, "42\n41\n20\n");
}
#[test]
fn inactive_macro_import_is_not_loaded_or_executed() {
    let f = Fixture::new(&[(
        "main.rfl",
        r#"
#[cfg(target="native")]
import macro {absent} from "./does-not-exist.rfl";
#[cfg(target="native")]
fn inactive() { absent! { raw text isn't RFL @ 🙂 }; }
fn main() { println(42); }
"#,
    )]);
    rfl::check_file(&f.main().join("main.rfl")).unwrap();
}

#[test]
fn quotation_splices_have_hygienic_locals_and_evaluate_interpolation_once() {
    let f = Fixture::new(&[
        (
            "macro.rfl",
            r#"
import {TokenStream,splice} from "std/macro";
#[proc_macro]
export fn twice(input:TokenStream)->TokenStream {splice("{ let temporary=$0; temporary+temporary }",[input])}
"#,
        ),
        (
            "main.rfl",
            r#"
import macro {twice} from "./macro.rfl";
fn main(){let temporary=99;let mut visits=0;println(twice!{{visits+=1;21}});println(visits);println(temporary);println(twice!{2});}
"#,
        ),
    ]);
    let out = rfl::run_native_file(&f.0.join("main.rfl"), false).unwrap();
    assert_eq!(out.stdout, "42\n1\n99\n4\n");
}
#[test]
fn item_macro_exports_an_explicit_call_site_function_name() {
    let f = Fixture::new(&[
        (
            "macro.rfl",
            r#"
import {TokenStream,splice,call_site_ident} from "std/macro";
#[proc_macro]
export fn declare(input:TokenStream)->TokenStream {splice("fn $0()->i16 {42}",[call_site_ident("answer")])}
"#,
        ),
        (
            "main.rfl",
            r#"
import macro {declare} from "./macro.rfl";
declare![];
fn main(){println(answer());}
"#,
        ),
    ]);
    assert_eq!(
        rfl::run_native_file(&f.0.join("main.rfl"), false)
            .unwrap()
            .stdout,
        "42\n"
    );
}
#[test]
fn generated_reference_resolves_in_definition_module_and_not_caller() {
    let f = Fixture::new(&[
        (
            "macro.rfl",
            r#"
import {TokenStream,splice} from "std/macro";
fn helper(value:i16)->i16{value+2}
#[proc_macro]
export fn add(input:TokenStream)->TokenStream{splice("helper($0)",[input])}
"#,
        ),
        (
            "main.rfl",
            r#"
import macro {add} from "./macro.rfl";
fn helper(value:i16)->i16{0}
fn main(){let helper=1;println(add!{40});}
"#,
        ),
    ]);
    assert_eq!(
        rfl::run_native_file(&f.0.join("main.rfl"), false)
            .unwrap()
            .stdout,
        "42\n"
    );
}
#[test]
fn output_and_service_errors_are_explicit_and_do_not_poison_a_retry() {
    for (body, expected) in [
        ("quote(\"1 2\")", "trailing"),
        ("{println(42);quote(\"42\")}", "deterministic profile"),
    ] {
        let implementation = format!(
            r#"import {{TokenStream,quote}} from "std/macro";#[proc_macro]export fn bad(input:TokenStream)->TokenStream{{{body}}}"#
        );
        let f = Fixture::new(&[
            ("macro.rfl", &implementation),
            (
                "main.rfl",
                r#"import macro {bad} from "./macro.rfl";fn main(){println(bad!{});}"#,
            ),
        ]);
        let error = rfl::check_file(&f.0.join("main.rfl")).unwrap_err();
        assert!(error.detail.contains(expected), "{error}");
        std::fs::write(f.0.join("macro.rfl"),r#"import {TokenStream,quote} from "std/macro";#[proc_macro]export fn bad(input:TokenStream)->TokenStream{quote("42")}"#).unwrap();
        assert_eq!(
            rfl::run_native_file(&f.0.join("main.rfl"), false)
                .unwrap()
                .stdout,
            "42\n"
        );
    }
}
#[test]
fn interpolation_type_error_keeps_original_location() {
    let source="import macro {identity} from \"./identity.rfl\";\nfn main(){\n let n:i16=identity!{missing};\n}";
    let f = Fixture::new(&[("identity.rfl", IDENTITY), ("main.rfl", source)]);
    let e = rfl::check_file(&f.0.join("main.rfl")).unwrap_err();
    assert!(e.detail.contains("missing"), "{e}");
    assert_eq!(e.offset, Some(source.find("missing").unwrap()));
    assert_eq!(e.line, 3);
}

#[test]
fn reexports_targets_and_runtime_phase_separation() {
    let f = Fixture::new(&[
        ("identity.rfl", IDENTITY),
        (
            "facade.rfl",
            r#"export import macro {identity as keep} from "./identity.rfl";"#,
        ),
        (
            "main.rfl",
            r#"import macro {keep as value} from "./facade.rfl";fn main(){println(value!{42});}"#,
        ),
    ]);
    assert_eq!(
        rfl::run_native_file(&f.0.join("main.rfl"), false)
            .unwrap()
            .stdout,
        "42\n"
    );
    std::fs::write(
        f.0.join("main.rfl"),
        r#"import {identity} from "./identity.rfl";fn main(){}"#,
    )
    .unwrap();
    assert!(rfl::check_file(&f.0.join("main.rfl"))
        .unwrap_err()
        .detail
        .contains("compile-time macro"));
    std::fs::write(f.0.join("identity.rfl"),r#"import {TokenStream,quote,caller_target} from "std/macro";#[proc_macro]export fn identity(input:TokenStream)->TokenStream{if(caller_target(input)=="ripple"){quote("42")}else{quote("43")}}"#).unwrap();
    std::fs::write(
        f.0.join("main.rfl"),
        r#"import macro {identity} from "./identity.rfl";fn main(){println(identity!{});}"#,
    )
    .unwrap();
    assert_eq!(
        rfl::run_native_file(&f.0.join("main.rfl"), false)
            .unwrap()
            .stdout,
        "43\n"
    );
    assert_eq!(rfl::run_file(&f.0.join("main.rfl")).unwrap().stdout, "42\n");
}
#[test]
fn malformed_signatures_visibility_cycles_and_metadata_are_rejected() {
    for (implementation,expected) in [
        ("#[proc_macro]export fn identity(n:i16)->i16{n}","TokenStream"),
        ("#[proc_macro]fn identity(input:TokenStream)->TokenStream{input}","not an exported"),
        ("#[proc_macro]export fn identity<T>(input:TokenStream)->TokenStream{input}","non-generic"),
        ("#[proc_macro]export fn identity(input:TokenStream)->TokenStream{TokenStream{tokens:[Token{kind:1,text:\"value\",origin:\"forged\",syntax:\"call\"}],target:\"\",dependencies:[]}}","invalid/stale source span"),
    ] {
        let source=format!("import {{Token,TokenStream}} from \"std/macro\";{implementation}");
        let f=Fixture::new(&[("identity.rfl",&source),("main.rfl",r#"import macro {identity} from "./identity.rfl";fn main(){println(identity!{42});}"#)]);
        let e=rfl::check_file(&f.0.join("main.rfl")).unwrap_err();assert!(e.to_string().contains(expected),"{e}");
    }
    let f = Fixture::new(&[
        (
            "identity.rfl",
            r#"export import macro {identity} from "./identity.rfl";"#,
        ),
        (
            "main.rfl",
            r#"import macro {identity} from "./identity.rfl";fn main(){}"#,
        ),
    ]);
    assert!(rfl::check_file(&f.0.join("main.rfl"))
        .unwrap_err()
        .detail
        .contains("cycle"));
}
#[test]
fn formatting_preserves_raw_macro_body_and_is_idempotent() {
    let source="import macro{html}from \"./html\";fn main(){let view=html! { <p title=\"🙂\"> isn't   ordinary @ RFL {name} </p> };}";
    let formatted = rfl::format_source(source).unwrap();
    assert!(
        formatted.contains("{ <p title=\"🙂\"> isn't   ordinary @ RFL {name} </p> }"),
        "{formatted}"
    );
    assert_eq!(rfl::format_source(&formatted).unwrap(), formatted);
}

#[test]
fn experimental_html_is_typed_user_code_with_native_rvm_parity() {
    let example = Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/macros/main.rfl");
    let expected="<div class=\"demo\">isn't it fun? 🙂<button>Ripple</button><strong class=\"badge\">typed</strong><br></br></div>\n1\n";
    let native = rfl::run_native_file(&example, false).unwrap();
    assert_eq!(native.status, 0);
    assert_eq!(native.stdout, expected);
    let ripple = rfl::run_file(&example).unwrap();
    assert_eq!(ripple.status, 0);
    assert_eq!(ripple.stdout, expected);
}
#[test]
fn declared_inputs_invalidate_artifacts_and_outputs_without_stale_spans() {
    let f = Fixture::new(&[
        (
            "macro.rfl",
            r#"import {TokenStream,declared_input,literal,diagnostic,call_span} from "std/macro";#[proc_macro]export fn value(input:TokenStream)->TokenStream{match declared_input(input,"number.txt"){Ok(dep)=>literal(dep.text),Err(message)=>diagnostic(call_span(),message)}}"#,
        ),
        ("macro.rfl.inputs", "number.txt\n"),
        ("number.txt", "42"),
        (
            "main.rfl",
            r#"import macro {value} from "./macro.rfl";fn main(){println(value!{});}"#,
        ),
    ]);
    for expected in ["42", "43", "43"] {
        std::fs::write(f.0.join("number.txt"), expected).unwrap();
        assert_eq!(
            rfl::run_native_file(&f.0.join("main.rfl"), false)
                .unwrap()
                .stdout,
            format!("{expected}\n")
        );
    }
    std::fs::write(f.0.join("macro.rfl.inputs"), "").unwrap();
    assert!(rfl::check_file(&f.0.join("main.rfl"))
        .unwrap_err()
        .detail
        .contains("undeclared macro input"));
}
#[test]
fn once_closures_owned_captures_and_generated_comprehensions_keep_language_rules() {
    let f = Fixture::new(&[
        ("identity.rfl", IDENTITY),
        (
            "main.rfl",
            r#"
import macro {identity} from "./identity.rfl";
import {map_once} from "std/fp";
fn id<T>(value:T)->T{value}
fn main(){
 let owner=owned(41,fn(value:&mut i16)=>{println("released");});
 let result=map_once<Option,_,_>(Some(1),identity!{once fn(n:i16)=>*owner.borrow()+id(n)});
 println(result);
 let skipped=owned("unused",fn(value:&mut String)=>{println("skipped");});
 let empty:Option<i16> = None;
 println(map_once<Option,_,_>(empty,identity!{once fn(n:i16)=>{skipped.borrow();n}}));
 println(identity!{for {n<-Some(40);yield n+2}});
}
"#,
        ),
    ]);
    let native = rfl::run_native_file(&f.0.join("main.rfl"), false).unwrap();
    assert_eq!(
        native.stdout,
        "released\nSome(42)\nskipped\nNone\nSome(42)\n"
    );
    assert_eq!(
        rfl::run_file(&f.0.join("main.rfl")).unwrap().stdout,
        native.stdout
    );
    let source = r#"import macro {identity} from "./identity.rfl";fn main(){let owner=owned(1,fn(n:&mut i16)=>{});let other=identity!{move(owner)};owner.borrow();}"#;
    std::fs::write(f.0.join("main.rfl"), source).unwrap();
    let e = rfl::check_file(&f.0.join("main.rfl")).unwrap_err();
    assert!(e.detail.contains("moved value"), "{e}");
    assert_eq!(e.offset, Some(source.rfind("owner.borrow").unwrap()));
    std::fs::write(f.0.join("main.rfl"),r#"import macro {identity} from "./identity.rfl";fn main(){let x=for{n<-Some(1);yield identity!{{return 42;0}}};}"#).unwrap();
    let e = rfl::check_file(&f.0.join("main.rfl")).unwrap_err();
    assert!(e.detail.contains("comprehension continuation"), "{e}");
}
#[test]
fn recursive_output_and_runaway_user_code_are_bounded() {
    for (body, expected) in [
        (r#"quote("identity!{}")"#, "depth limit"),
        ("{while(true){}input}", "macro executable failed"),
    ] {
        let implementation = format!(
            r#"import {{TokenStream,quote}} from "std/macro";#[proc_macro]export fn identity(input:TokenStream)->TokenStream{{{body}}}"#
        );
        let f = Fixture::new(&[
            ("identity.rfl", &implementation),
            (
                "main.rfl",
                r#"import macro {identity} from "./identity.rfl";fn main(){identity!{};}"#,
            ),
        ]);
        let e = rfl::check_file(&f.0.join("main.rfl")).unwrap_err();
        assert!(e.to_string().contains(expected), "{e}");
    }
}
#[test]
fn a_macro_program_runs_in_the_actual_compact_brainfuck_cpu() {
    let f = Fixture::new(&[
        ("identity.rfl", IDENTITY),
        (
            "main.rfl",
            r#"import macro {identity} from "./identity.rfl";fn main()->i16{identity!{37}}"#,
        ),
    ]);
    // Reuse the compact object/export helper, with an explicit host deadline.
    let compact = support::Fixture(f.0.join("main.rfl"));
    let object = support::object(&compact);
    let image = f.0.join("image.bf");
    support::tool(
        std::process::Command::new(support::root().join("src/ripple-asm/target/release/rlink"))
            .arg(support::root().join("runtime/crt0.pobj"))
            .arg(object)
            .arg(support::root().join("runtime/libruntime.par"))
            .args([
                "-f",
                "bf",
                "--standalone",
                "--bf-host",
                "--memory-banks",
                "16",
                "--bank-size",
                "64000",
                "-o",
            ])
            .arg(&image),
    );
    assert!(std::fs::metadata(&image).unwrap().len() < 20_000_000);
    let output = std::process::Command::new(support::root().join("target/release/rbf"))
        .arg(image)
        .args(["--jit", "--headless", "--timeout", "30", "--exit-code"])
        .output()
        .unwrap();
    assert_eq!(
        output.status.code(),
        Some(37),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn html_preserves_interpolation_order_and_reports_typed_and_syntax_errors_at_source() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/macros");
    let implementation = std::fs::read_to_string(root.join("html_macro.rfl")).unwrap();
    let tree = std::fs::read_to_string(root.join("tree.rfl")).unwrap();
    let prefix = r#"import macro {html} from "./html_macro.rfl";import {Node,text,node,render} from "./tree.rfl";type Msg=Click;fn Badge(label:String,children:Vec<Node<Msg>>)->Node<Msg>{node(label,[],children)}"#;
    let source = format!(
        r#"{prefix}fn main(){{let mut order="";let view:Node<Msg> = html!{{<div a={{{{order=order+"a";"A"}}}} b={{{{order=order+"b";"B"}}}}>{{{{order=order+"c";text("C")}}}}<Badge label={{{{order=order+"d";"strong"}}}}>{{{{order=order+"e";text("E")}}}}</Badge>/* discarded */ isn't @ 🙂</div>}};println(order);println(render(view));}}"#
    );
    let f = Fixture::new(&[
        ("html_macro.rfl", &implementation),
        ("tree.rfl", &tree),
        ("main.rfl", &source),
    ]);
    let native = rfl::run_native_file(&f.0.join("main.rfl"), false).unwrap();
    assert_eq!(
        native.stdout,
        "abcde\n<div a=\"A\" b=\"B\">C<strong>E</strong> isn't @ 🙂</div>\n"
    );
    assert_eq!(
        rfl::run_file(&f.0.join("main.rfl")).unwrap().stdout,
        native.stdout
    );
    for (dsl, message) in [
        ("<div></span>", "does not match"),
        ("<div title={42}/>", "String"),
        ("<div>{missing}</div>", "missing"),
        ("<div on_click={42}/>", "integer literal"),
    ] {
        let source = format!("{prefix}\nfn main(){{let view:Node<Msg> = html!{{{dsl}}};}}");
        std::fs::write(f.0.join("main.rfl"), &source).unwrap();
        let e = rfl::check_file(&f.0.join("main.rfl")).unwrap_err();
        assert!(e.detail.contains(message), "{e}");
        assert_eq!(e.line, 2, "{e}");
        assert!(e.message.contains("expan"), "{e}");
    }
}
#[test]
fn unsupported_positions_generated_imports_protocol_forgery_and_output_limits_are_explicit() {
    let f = Fixture::new(&[("macro.rfl", IDENTITY), ("main.rfl", "")]);
    for source in [
        r#"import macro {identity} from "./macro.rfl";fn main(){let x:identity!{i16}=1;}"#,
        r#"import macro {identity} from "./macro.rfl";fn main(){let identity!{x}=1;}"#,
    ] {
        std::fs::write(f.0.join("main.rfl"), source).unwrap();
        let e = rfl::check_file(&f.0.join("main.rfl")).unwrap_err();
        assert!(
            e.detail.contains("macro") && e.detail.contains("position"),
            "{e}"
        );
    }
    for (body, expected) in [
        (r#"quote("import {text} from \"std/string\";")"#, "imports"),
        (
            r#"{let mut token=input.tokens[0];token.syntax="forged";TokenStream{tokens:[token],target:"",dependencies:[]}}"#,
            "syntax context",
        ),
        (r#"group("()",empty())"#, "delimiter"),
        (
            r#"{let mut result=empty();for(n in 0..2500){result.tokens.push(Token{kind:10,text:"42",origin:"call",syntax:"definition"});}result}"#,
            "protocol",
        ),
    ] {
        let implementation = format!(
            r#"import {{Token,TokenStream,quote,group,empty,concat}} from "std/macro";#[proc_macro]export fn identity(input:TokenStream)->TokenStream{{{body}}}"#
        );
        std::fs::write(f.0.join("macro.rfl"), implementation).unwrap();
        std::fs::write(
            f.0.join("main.rfl"),
            r#"import macro {identity} from "./macro.rfl";identity!{42};fn main(){}"#,
        )
        .unwrap();
        let e = rfl::check_file(&f.0.join("main.rfl")).unwrap_err();
        assert!(e.to_string().contains(expected), "{e}");
    }
}

#[test]
fn definition_site_macro_aliases_and_constructor_helpers_do_not_pollute_the_caller() {
    let f = Fixture::new(&[
        (
            "inner.rfl",
            r#"import {TokenStream,quote} from "std/macro";#[proc_macro]export fn inner(input:TokenStream)->TokenStream{quote("42")}"#,
        ),
        (
            "outer.rfl",
            r#"import {TokenStream,quote} from "std/macro";import macro {inner as helper} from "./inner.rfl";#[proc_macro]export fn outer(input:TokenStream)->TokenStream{quote("helper!{}")}"#,
        ),
        (
            "values.rfl",
            r#"export type Choice=Empty|Full(i16);export fn make()->Choice{Empty}"#,
        ),
        (
            "match.rfl",
            r#"import {TokenStream,splice} from "std/macro";import {Choice} from "./values.rfl";#[proc_macro]export fn value(input:TokenStream)->TokenStream{splice("match($0){Empty=>42,Full(n)=>n}",[input])}"#,
        ),
        (
            "main.rfl",
            r#"import macro {outer} from "./outer.rfl";import macro {inner as helper} from "./inner.rfl";import macro {value} from "./match.rfl";import {make} from "./values.rfl";type Own=Empty|Full(i16);fn main(){println(outer!{});println(value!{make()});println(value!{make()});let local:Own=Full(3);}"#,
        ),
    ]);
    assert_eq!(
        rfl::run_native_file(&f.0.join("main.rfl"), false)
            .unwrap()
            .stdout,
        "42\n42\n42\n"
    );
    assert_eq!(
        rfl::run_file(&f.0.join("main.rfl")).unwrap().stdout,
        "42\n42\n42\n"
    );
    let ir = rfl::compile_file(&f.0.join("main.rfl")).unwrap();
    assert!(
        !ir.functions.iter().any(|f| f.name.contains("quote")
            || f.name.contains("splice")
            || f.name.contains("macro_dispatch")),
        "macro-host implementation leaked into target IR"
    );
}

#[test]
fn fresh_identifier_families_and_item_expansions_remain_distinct_across_reused_artifacts() {
    let f = Fixture::new(&[
        (
            "macro.rfl",
            r#"import {TokenStream,splice,fresh_ident,concat,quote,joint_punctuation,punctuation} from "std/macro";#[proc_macro]export fn value(input:TokenStream)->TokenStream{splice("{let $0=20;let $1=22;$0+$1}",[fresh_ident("temporary","a"),fresh_ident("temporary","b")])}#[proc_macro]export fn declare(input:TokenStream)->TokenStream{splice("fn private_helper()->i16{42} fn $0()->i16{private_helper()}",[input])}#[proc_macro]export fn shift(input:TokenStream)->TokenStream{concat(quote("64"),concat(joint_punctuation(">"),concat(punctuation(">"),quote("1"))))}"#,
        ),
        (
            "main.rfl",
            r#"import macro {value,declare,shift} from "./macro.rfl";declare!{one};declare!{two};fn main(){println(value!{});println(value!{});println(one()+two());println(shift!{});}"#,
        ),
    ]);
    for _ in 0..2 {
        assert_eq!(
            rfl::run_native_file(&f.0.join("main.rfl"), false)
                .unwrap()
                .stdout,
            "42\n42\n84\n32\n"
        );
    }
    assert_eq!(
        rfl::run_file(&f.0.join("main.rfl")).unwrap().stdout,
        "42\n42\n84\n32\n"
    );
    std::fs::write(f.0.join("main.rfl"),r#"import macro {value} from "./macro.rfl";#[proc_macro]fn invalid(input:i16)->i16{input}fn main(){invalid!{};}"#).unwrap();
    let e = rfl::check_file(&f.0.join("main.rfl")).unwrap_err();
    assert!(e.detail.contains("same-module"), "{e}");
}

#[test]
fn empty_item_expansion_is_valid_but_empty_expression_and_standalone_items_are_errors() {
    let f = Fixture::new(&[
        (
            "macro.rfl",
            r#"import {TokenStream,empty} from "std/macro";#[proc_macro]export fn discard(input:TokenStream)->TokenStream{empty()}"#,
        ),
        (
            "main.rfl",
            r#"import macro {discard} from "./macro.rfl";discard!{raw isn't @ RFL 🙂};fn main(){println(42);}"#,
        ),
    ]);
    assert_eq!(
        rfl::run_native_file(&f.0.join("main.rfl"), false)
            .unwrap()
            .stdout,
        "42\n"
    );
    let source = "import macro {discard} from \"./macro.rfl\";\nfn main(){let value=discard!{};}";
    std::fs::write(f.0.join("main.rfl"), source).unwrap();
    let error = rfl::check_file(&f.0.join("main.rfl")).unwrap_err();
    assert_eq!(error.line, 2, "{error}");
    assert_eq!(error.offset, Some(source.find("discard!").unwrap()));
    assert!(rfl::check_source("unknown!{} fn main(){}")
        .unwrap_err()
        .detail
        .contains("source file"));
}

#[test]
fn structured_literals_use_the_normal_rfl_literal_grammar() {
    let f = Fixture::new(&[
        (
            "macro.rfl",
            r#"import {TokenStream,literal} from "std/macro";#[proc_macro]export fn boolean(input:TokenStream)->TokenStream{literal("true")}#[proc_macro]export fn number(input:TokenStream)->TokenStream{literal("1e-2")}"#,
        ),
        (
            "main.rfl",
            r#"import macro {boolean,number} from "./macro.rfl";fn main(){println(boolean!{});let n:f32=number!{};println(n>0.009 && n<0.011);}"#,
        ),
    ]);
    assert_eq!(
        rfl::run_native_file(&f.0.join("main.rfl"), false)
            .unwrap()
            .stdout,
        "true\ntrue\n"
    );
    assert_eq!(
        rfl::run_file(&f.0.join("main.rfl")).unwrap().stdout,
        "true\ntrue\n"
    );
}

#[test]
fn runtime_helper_parameters_may_shadow_compiler_api_import_names() {
    let f = Fixture::new(&[
        (
            "macro.rfl",
            r#"import {TokenStream,quote,splice} from "std/macro";fn helper(quote:i16)->i16{quote+2}#[proc_macro]export fn add(input:TokenStream)->TokenStream{splice("helper($0)",[input])}"#,
        ),
        (
            "main.rfl",
            r#"import macro {add} from "./macro.rfl";fn main(){println(add!{40});}"#,
        ),
    ]);
    assert_eq!(
        rfl::run_native_file(&f.0.join("main.rfl"), false)
            .unwrap()
            .stdout,
        "42\n"
    );
}

#[test]
fn macro_only_implementation_imports_do_not_become_runtime_dependencies() {
    let f = Fixture::new(&[
        (
            "build.rfl",
            r#"import {TokenStream,splice} from "std/macro";export fn build(input:TokenStream)->TokenStream{splice("helper($0)",[input])}"#,
        ),
        (
            "macro.rfl",
            r#"import {TokenStream} from "std/macro";import {build} from "./build.rfl";fn helper(n:i16)->i16{n+2}#[proc_macro]export fn add(input:TokenStream)->TokenStream{build(input)}"#,
        ),
        (
            "main.rfl",
            r#"import macro {add} from "./macro.rfl";fn main(){println(add!{40});}"#,
        ),
    ]);
    assert_eq!(
        rfl::run_native_file(&f.0.join("main.rfl"), false)
            .unwrap()
            .stdout,
        "42\n"
    );
    assert_eq!(rfl::run_file(&f.0.join("main.rfl")).unwrap().stdout, "42\n");
    let ir = rfl::compile_file(&f.0.join("main.rfl")).unwrap();
    assert!(!ir
        .functions
        .iter()
        .any(|f| f.name.contains("splice") || f.name.contains("quote")));
}

#[test]
fn generated_declarations_resolve_each_other_before_definition_site_or_prelude_names() {
    let f = Fixture::new(&[
        (
            "macro.rfl",
            r#"import {TokenStream,splice} from "std/macro";fn helper()->i16{0}#[proc_macro]export fn declare(input:TokenStream)->TokenStream{splice("type Choice=None|Some(i16);fn helper()->i16{match Some(42){Some(n)=>n,None=>0}}fn $0()->i16{helper()}",[input])}"#,
        ),
        (
            "main.rfl",
            r#"import macro {declare} from "./macro.rfl";declare!{one};declare!{two};fn main(){println(one());println(two());}"#,
        ),
    ]);
    assert_eq!(
        rfl::run_native_file(&f.0.join("main.rfl"), false)
            .unwrap()
            .stdout,
        "42\n42\n"
    );
    assert_eq!(
        rfl::run_file(&f.0.join("main.rfl")).unwrap().stdout,
        "42\n42\n"
    );
}

#[test]
fn bootstrap_macros_can_generate_macro_implementations_and_track_transitive_declared_inputs() {
    let f = Fixture::new(&[
        (
            "builder.rfl",
            r#"import {TokenStream,quote,declared_input,diagnostic,call_span} from "std/macro";#[proc_macro]export fn body(input:TokenStream)->TokenStream{match declared_input(input,"number.txt"){Ok(dep)=>quote("quote(\""+dep.text+"\")"),Err(error)=>diagnostic(call_span(),error)}}"#,
        ),
        ("builder.rfl.inputs", "number.txt\n"),
        ("number.txt", "42"),
        (
            "macro.rfl",
            r#"import {TokenStream} from "std/macro";import macro {body} from "./builder.rfl";#[proc_macro]export fn value(input:TokenStream)->TokenStream{body!{}}"#,
        ),
        (
            "main.rfl",
            r#"import macro {value} from "./macro.rfl";fn main(){println(value!{});}"#,
        ),
    ]);
    for number in ["42", "43"] {
        std::fs::write(f.0.join("number.txt"), number).unwrap();
        assert_eq!(
            rfl::run_native_file(&f.0.join("main.rfl"), false)
                .unwrap()
                .stdout,
            format!("{number}\n")
        );
    }
    assert_eq!(rfl::run_file(&f.0.join("main.rfl")).unwrap().stdout, "43\n");
}

#[test]
fn primitive_named_generated_locals_do_not_capture_caller_values() {
    for name in [
        "i16",
        "String",
        "Vec",
        "Option",
        "putc",
        "from_char",
        "drop",
        "move",
        "println",
    ] {
        let implementation = r#"import {TokenStream,splice} from "std/macro";#[proc_macro]export fn keep(input:TokenStream)->TokenStream{splice("{let BINDING=1;$0+BINDING}",[input])}"#.replace("BINDING", name);
        let caller = r#"import macro {keep} from "./macro.rfl";import {println as out} from "std/io";fn main(){let BINDING=41;out(keep!{BINDING});out(BINDING);}"#.replace("BINDING", name);
        let f = Fixture::new(&[("macro.rfl", &implementation), ("main.rfl", &caller)]);
        assert_eq!(
            rfl::run_native_file(&f.0.join("main.rfl"), false)
                .unwrap()
                .stdout,
            "42\n41\n",
            "{name}"
        );
        assert_eq!(
            rfl::run_file(&f.0.join("main.rfl")).unwrap().stdout,
            "42\n41\n",
            "{name}"
        );
    }
}

#[test]
fn generated_methods_use_definition_traits_and_spliced_methods_use_caller_traits() {
    let f = Fixture::new(&[
        (
            "values.rfl",
            r#"export trait Display {fn display(&self)->i16;} impl Display for i16 {fn display(&self)->i16{*self+2}}"#,
        ),
        (
            "macro.rfl",
            r#"import {TokenStream,splice} from "std/macro";import {Display} from "./values.rfl";#[proc_macro]export fn render(input:TokenStream)->TokenStream{splice("($0).display()",[input])}"#,
        ),
        (
            "main.rfl",
            r#"import macro {render} from "./macro.rfl";trait Caller {fn display(&self)->i16;}impl Caller for i16 {fn display(&self)->i16{*self+100}}fn main(){println(render!{40});println(render!{(1 as i16).display()});println((1 as i16).display());}"#,
        ),
    ]);
    assert_eq!(
        rfl::run_native_file(&f.0.join("main.rfl"), false)
            .unwrap()
            .stdout,
        "42\n103\n101\n"
    );
    assert_eq!(
        rfl::run_file(&f.0.join("main.rfl")).unwrap().stdout,
        "42\n103\n101\n"
    );
}

#[test]
fn expected_method_results_reach_generic_receivers_without_losing_ordinary_argument_inference() {
    for fixture in ["method-result-receiver.rfl", "method-result-branches.rfl"] {
        let path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures")
            .join(fixture);
        assert_eq!(rfl::run_native_file(&path, false).unwrap().stdout, "42\n");
        assert_eq!(rfl::run_file(&path).unwrap().stdout, "42\n");
    }
}

#[test]
fn generated_traits_and_associated_projections_keep_their_lexical_scope() {
    let f = Fixture::new(&[
        (
            "values.rfl",
            r#"export trait Display {type Output;fn display(&self)->Self::Output;}impl Display for i16 {type Output=i16;fn display(&self)->i16{*self+2}}"#,
        ),
        (
            "macro.rfl",
            r#"
import {TokenStream,splice} from "std/macro";
import {Display} from "./values.rfl";
#[proc_macro]export fn local(input:TokenStream)->TokenStream{splice("trait Local {type Output;fn value(&self)->Self::Output;} impl Local for i16 {type Output=i16;fn value(&self)->i16{*self+2}} fn project<T:Local>(n:&T)->T::Output{n.value()} fn $0()->i16{let n:i16=40;project(&n)}",[input])}
#[proc_macro]export fn imported(input:TokenStream)->TokenStream{splice("fn project<T:Display>(n:&T)->T::Output{n.display()} fn $0()->i16{let n:i16=40;project(&n)}",[input])}
"#,
        ),
        (
            "main.rfl",
            r#"import macro {local,imported} from "./macro.rfl";local!{one};imported!{two};fn main(){println(one());println(two());}"#,
        ),
    ]);
    assert_eq!(
        rfl::run_native_file(&f.0.join("main.rfl"), false)
            .unwrap()
            .stdout,
        "42\n42\n"
    );
    assert_eq!(
        rfl::run_file(&f.0.join("main.rfl")).unwrap().stdout,
        "42\n42\n"
    );
}
