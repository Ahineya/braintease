use rfl::{format_edits, format_source};

const IMPORT: &str = "import macro {html} from \"std/html_macro\";";

fn checked(source: &str) -> String {
    let result = format_source(source).unwrap();
    assert_eq!(format_source(&result).unwrap(), result);
    let mut edited = source.to_owned();
    for edit in format_edits(source).unwrap().into_iter().rev() {
        assert!(source[edit.start..edit.end]
            .chars()
            .all(char::is_whitespace));
        assert!(edit.new_text.chars().all(char::is_whitespace));
        edited.replace_range(edit.start..edit.end, &edit.new_text);
    }
    assert_eq!(edited, result);
    result
}

#[test]
fn html_trees_and_attributes_receive_nested_layout() {
    let text = checked(&format!("{IMPORT}fn view(){{html!{{<div><input class=\"edit\" value={{title}} on_input={{fn(s:String)=>Edit(s)}}/><p>Hello, {{name}}!</p></div>}}}}"));
    assert!(
        text.contains("html!{\n        <div>\n            <input\n"),
        "{text}"
    );
    assert!(
        text.contains("on_input={fn(s: String) => Edit(s)}"),
        "{text}"
    );
    assert!(text.contains("<p>Hello, {name}!</p>"), "{text}");
}

#[test]
fn significant_text_and_preserved_elements_are_byte_exact() {
    let raw = "<pre> a\n  b <span> c </span>\n</pre>";
    let text = checked(&format!("{IMPORT}fn view(){{html!{{<div>{raw}<textarea> first\n second </textarea><p><strong>A</strong> <em>B</em></p></div>}}}}"));
    assert!(text.contains(raw), "{text}");
    assert!(
        text.contains("<textarea> first\n second </textarea>"),
        "{text}"
    );
    assert!(
        text.contains("<p><strong>A</strong> <em>B</em></p>"),
        "{text}"
    );
}

#[test]
fn aliases_fragments_components_svg_and_crlf_are_supported() {
    let text = checked("import macro {html as view} from \"std/html_macro\";\r\nfn main(){view![<><Badge label={title}><svg><path d=\"M0 0\"/></svg></Badge>{rows}</>];}\r\n");
    assert!(text.contains("view![\r\n"), "{text}");
    assert!(text.contains("\r\n            <Badge"), "{text}");
    assert!(!text.replace("\r\n", "").contains('\n'));
}

#[test]
fn arbitrary_macros_keep_lossless_bodies_even_when_named_html() {
    let body = "{ <p title=\"🙂\"> isn't   ordinary @ RFL {name} </p> }";
    let text = checked(&format!(
        "import macro {{html}} from \"./custom\";fn main(){{html!{body};}}"
    ));
    assert!(text.contains(body));
}

#[test]
fn formatting_has_no_macro_execution_or_import_resolution() {
    let text = checked(&format!(
        "{IMPORT}import {{absent}} from \"./missing\";fn main(){{html!{{<div>{{missing}}</div>}}}}"
    ));
    assert!(text.contains("html!{\n"), "{text}");
}

#[test]
fn comments_literals_and_nested_expression_blocks_survive() {
    let text = checked(&format!(
        r#"{IMPORT}fn view(){{html!{{<button title="a > b" on_click={{{{let n=1;fn(event:Event)=>{{if(event.key=="Enter"){{Click(n)}}else{{Ignore}}}}}}}}>isn't 🙂 /* keep */ {{value}}</button>}}}}"#
    ));
    assert!(text.contains("title=\"a > b\""));
    assert!(text.contains("isn't 🙂 /* keep */ {value}"));
    assert!(text.contains("let n = 1;"), "{text}");
}

#[test]
fn compiled_html_text_is_identical_before_and_after_formatting() {
    let source = r#"
import macro {html} from "std/html_macro";
import {Html,HtmlText,HtmlElement,HtmlFragment} from "std/html";
fn render(node:Html<i16>)->String {
 match node {
  HtmlText(value)=>"["+value+"]",
  HtmlElement(_,tag,_,children)=>{let mut out="<"+tag+">";for(child in children){out=out+render(child);}out+"</"+tag+">"},
  HtmlFragment(children)=>{let mut out="";for(child in children){out=out+render(child);}out}
 }
}
fn main(){let name="Ripple";let tree:Html<i16> =html!{<section><div><p>Hello <strong>{name}</strong> !</p><pre> a
  b <span> c </span>
</pre><textarea> first
 second </textarea><span>A</span> <span>B</span></div><footer>done</footer></section>};println(render(tree));}
"#;
    let formatted = checked(source);
    let root = std::env::temp_dir().join(format!("rfl-html-text-{}", std::process::id()));
    std::fs::create_dir_all(&root).unwrap();
    let file = root.join("main.rfl");
    let mut outputs = Vec::new();
    for text in [source, formatted.as_str()] {
        std::fs::write(&file, text).unwrap();
        let native = rfl::run_native_file(&file, false).unwrap();
        assert_eq!(native.status, 0);
        assert_eq!(rfl::run_file(&file).unwrap().stdout, native.stdout);
        outputs.push(native.stdout);
    }
    std::fs::remove_dir_all(root).unwrap();
    assert_eq!(outputs[0], outputs[1]);
    assert!(outputs[0].contains("<p>[Hello ]<strong>[Ripple]</strong>[ !]</p>"));
    assert!(outputs[0].contains("<span>[A]</span>[ ]<span>[B]</span>"));
    assert!(outputs[0].contains("<pre>[ a\n  b ]<span>[ c ]</span>[\n]</pre>"));
}

#[test]
fn nested_html_and_multiline_comments_keep_their_own_syntax() {
    let source = format!(
        r#"{IMPORT}fn main(){{html!{{<div>{{if(ok){{html!{{<span><strong>yes</strong></span>}}}}else{{html!{{<em>no</em>}}}}}}</div>}}}}"#
    );
    let text = checked(&source);
    assert!(text.contains("<span>\n"), "{text}");
    let comment = "/* keep\n    these exact spaces */";
    let text = checked(&format!(
        "{IMPORT}fn main(){{html!{{<p>{{{{{comment}value}}}}</p>}}}}"
    ));
    assert!(text.contains(comment));
}

#[test]
fn embedded_opaque_macros_and_nested_verbatim_html_are_not_reindented() {
    for value in [
        "raw!{a\n    b}",
        "html!{<pre>one\n    two</pre>}",
        "html!{<p>one\n    two</p>}",
    ] {
        let output = checked(&format!(
            "{IMPORT}fn main(){{html!{{<div>{{{value}}}</div>}}}}"
        ));
        assert!(output.contains(value), "{output}");
    }
}

#[test]
fn wasm_only_html_imports_get_layout_without_resolving_dependencies() {
    let text = checked("#[cfg(target=\"wasm\")]import macro {html as view} from \"std/html_macro\";#[cfg(target=\"wasm\")]fn main(){view!{<section><p>Hello</p></section>}}");
    assert!(
        text.contains("import macro { html as view } from"),
        "{text}"
    );
    assert!(text.contains("view!{\n        <section>\n"), "{text}");
}

#[test]
fn target_dependent_aliases_cannot_reformat_another_macro_packages_dsl() {
    let body = "{<section><p>keep exact whitespace</p></section>}";
    let source = format!("#[cfg(target=\"wasm\")]import macro {{html}} from \"std/html_macro\";#[cfg(target=\"native\")]import macro {{html}} from \"./another-dsl\";#[cfg(target=\"wasm\")]fn browser(){{html!{body}}}#[cfg(target=\"native\")]fn host(){{html!{body}}}");
    let text = checked(&source);
    assert_eq!(text.matches(body).count(), 2, "{text}");
}
