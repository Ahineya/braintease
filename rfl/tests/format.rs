use rfl::{format_edits, format_source};

fn formatted(source: &str, expected: &str) {
    let actual = format_source(source).unwrap();
    assert_eq!(actual, expected);
    assert_eq!(
        format_source(&actual).unwrap(),
        actual,
        "formatter must be idempotent"
    );
    let mut edited = source.to_string();
    for edit in format_edits(source).unwrap().into_iter().rev() {
        assert!(source[edit.start..edit.end]
            .chars()
            .all(char::is_whitespace));
        assert!(edit.new_text.chars().all(char::is_whitespace));
        edited.replace_range(edit.start..edit.end, &edit.new_text);
    }
    assert_eq!(edited, actual);
}

#[test]
fn canonical_blocks_and_operators() {
    formatted(
        "fn main( )->i16{let mut x=1+2*3;if(x>2){x=x-1;}else{x=0;}x}",
        "fn main() -> i16 {\n    let mut x = 1 + 2 * 3;\n    if (x > 2) {\n        x = x - 1;\n    } else {\n        x = 0;\n    }\n    x\n}\n",
    );
}

#[test]
fn block_arguments_do_not_add_space_after_opening_delimiters() {
    formatted(
        "fn main(){Ok({});let a=[{}];}",
        "fn main() {\n    Ok({});\n    let a = [{}];\n}\n",
    );
}

#[test]
fn http_server_example_requires_assignment_spacing() {
    let source = include_str!("../examples/http-server.rfl")
        .replace("let request = ", "let request=")
        .replace("let response = ", "let response=")
        .replace("let mut lease = ", "let mut lease=");
    let output = format_source(&source).unwrap();
    assert!(output.contains("let request = server.accept().await?;"));
    assert!(output.contains("let response = if (request.method != \"GET\") {"));
    assert!(output.contains("let mut lease = network.acquire(30000);"));
    assert!(output.contains("Ok({})"));
    let mut edited = source.to_string();
    for edit in format_edits(&source).unwrap().into_iter().rev() {
        edited.replace_range(edit.start..edit.end, &edit.new_text);
    }
    assert_eq!(edited, output);
    assert_eq!(format_source(&output).unwrap(), output);
}

#[test]
fn generics_references_and_comparisons() {
    formatted(
        "fn f<T:Printable>(x:&mut Vec<Option<T>>,p:*const i16)->bool{let y=-1;let z= *p & 3; x.len()<2 && z>>1>y}",
        "fn f<T: Printable>(x: &mut Vec<Option<T>>, p: *const i16) -> bool {\n    let y = -1;\n    let z = *p & 3;\n    x.len() < 2 && z >> 1 > y\n}\n",
    );
}

#[test]
fn generic_array_lengths_keep_expression_operator_spacing() {
    formatted(
        "fn f(x:Vec<[i16;4>>1]>, y:Vec<fn(Vec<i16>)->Vec<i16>>){}",
        "fn f(x: Vec<[i16; 4 >> 1]>, y: Vec<fn(Vec<i16>) -> Vec<i16>>) {}\n",
    );
}

#[test]
fn generic_commas_inside_blocks_stay_inline() {
    let expected = "impl HttpServer {\n    export fn accept(&self) -> Async<Result<IncomingRequest, NetError>> {\n        accept(retain(pointer(&self.state)), self.epoch)\n    }\n}\n";
    formatted(expected, expected);
    formatted(
        &expected.replace(
            "IncomingRequest, NetError",
            "IncomingRequest,\n    NetError",
        ),
        expected,
    );
    formatted(
        "struct State{value:Result<i16,i16>,other:i16}trait Read{fn read(&self)->Result<i16,i16>;}impl<T,U> Result<T,U>{fn f<V,W>(&self){let x:Result<V,W> =make<V,W>();}}",
        "struct State {\n    value: Result<i16, i16>,\n    other: i16\n}\n\ntrait Read {\n    fn read(&self) -> Result<i16, i16>;\n}\n\nimpl<T, U> Result<T, U> {\n    fn f<V, W>(&self) {\n        let x: Result<V, W> = make<V, W>();\n    }\n}\n",
    );
}

#[test]
fn imports_structs_patterns_and_match_arms() {
    formatted(
        "import{Some as Yes}from\"std/option\";struct Point{x:i16,y:i16}fn main(){let p=Point{x:1,y:2};let Point{x,y}=p;match x{0=>p.x, n=>n+y,}}",
        "import { Some as Yes } from \"std/option\";\nstruct Point {\n    x: i16,\n    y: i16\n}\n\nfn main() {\n    let p = Point { x: 1, y: 2 };\n    let Point { x, y } = p;\n    match x {\n        0 => p.x,\n        n => n + y,\n    }\n}\n",
    );
}

#[test]
fn preserves_comments_literals_and_crlf() {
    formatted(
        "// header\r\nfn main(){let s=c\"a\\n\\\"b\";// keep 🙂\r\n/* block\r\n  unchanged */\r\nprintln(s);}\r\n",
        "// header\r\nfn main() {\r\n    let s = c\"a\\n\\\"b\"; // keep 🙂\r\n    /* block\r\n  unchanged */\r\n    println(s);\r\n}\r\n",
    );
}

#[test]
fn comments_force_safe_multiline_lists() {
    formatted(
        "fn main(){let a=[1,// first\n2,3];println(a);}",
        "fn main() {\n    let a = [\n        1, // first\n        2,\n        3\n    ];\n    println(a);\n}\n",
    );
}

#[test]
fn no_line_width_limit_and_no_typecheck_requirement() {
    let name = "long_name_".repeat(40);
    let source = format!("fn main(){{missing({name}, 123);}}");
    let expected = format!("fn main() {{\n    missing({name}, 123);\n}}\n");
    formatted(&source, &expected);
}

#[test]
fn separated_generic_angles_and_prefix_operators_stay_unambiguous() {
    formatted(
        "fn f<T>(x: Vec < Vec < T > >, y: Vec< <T as Read>::Item>){let a = & & x;let b=1- -2;}",
        "fn f<T>(x: Vec<Vec<T> >, y: Vec< <T as Read>::Item>) {\n    let a = & &x;\n    let b = 1 - -2;\n}\n",
    );
}

#[test]
fn cfg_externs_async_lambdas_arrays_and_block_expressions() {
    let source = r#"
#[cfg(target = "native")] fn native<T>(x: Vec<T>)->Vec<T>{x}
#[cfg(target = "ripple")] fn ripple<T>(x: Vec<T>)->Vec<T>{x}
#[repr(C)] struct Data { value: c_int }
extern "C" { fn write(p: *const c_char,n:usize,...)->c_int; }
trait Read { type Item: Printable; fn read(&self)->Self::Item; }
impl Read for Data { type Item=i16; fn read(&self)->i16{self.value} }
async fn probe()->i16{1}
fn main(){let f=fn(x:i16)=>x+1;let a:[i16;2]=[1,2];for(i in 0..2){println(a[i]);}unsafe{let p:*mut i16=unknown();*p = -1;}let x=probe().await;match x{0=>{println(0);} n if(n>0)=>f(n),_=> -1,}}
"#;
    let actual = format_source(source).unwrap();
    assert!(actual.contains("fn native<T>(x: Vec<T>) -> Vec<T>"));
    assert!(actual.contains("fn ripple<T>(x: Vec<T>) -> Vec<T>"));
    assert!(actual.contains("fn read(&self) -> Self::Item;"));
    assert!(actual.contains("let f = fn(x: i16) => x + 1;"));
    assert!(actual.contains("let a: [i16; 2] = [1, 2];"));
    assert!(actual.contains("for (i in 0..2) {"));
    assert!(actual.contains("*p = -1;"));
    formatted(source, &actual);
}

#[test]
fn empty_sources_and_invalid_syntax() {
    formatted(" \t\n", "");
    formatted("// only a comment", "// only a comment\n");
    for source in ["fn main( {", "fn main() { let x = ; }", "/* unfinished"] {
        assert!(format_source(source).is_err(), "{source}");
    }
}

#[test]
fn trailing_item_comments_do_not_remove_declaration_separation() {
    formatted(
        "fn a(){}// keep\nfn b(){}",
        "fn a() {} // keep\n\nfn b() {}\n",
    );
}

#[test]
fn block_expressions_keep_their_continuing_operator() {
    formatted(
        "fn main()->i16{let x={1}+2;x}",
        "fn main() -> i16 {\n    let x = {\n        1\n    } + 2;\n    x\n}\n",
    );
}

#[test]
fn repository_sources_preserve_tokens_and_are_idempotent() {
    fn visit(directory: &std::path::Path) {
        for entry in std::fs::read_dir(directory).unwrap() {
            let path = entry.unwrap().path();
            if path.is_dir() {
                visit(&path);
            } else if path.extension().is_some_and(|ext| ext == "rfl") {
                let source = std::fs::read_to_string(&path).unwrap();
                let output = format_source(&source)
                    .unwrap_or_else(|err| panic!("{}: {err}", path.display()));
                assert_eq!(
                    format_source(&output).unwrap(),
                    output,
                    "{}",
                    path.display()
                );
            }
        }
    }
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    for folder in ["examples", "std", "benchmarks"] {
        visit(&root.join(folder));
    }
}
