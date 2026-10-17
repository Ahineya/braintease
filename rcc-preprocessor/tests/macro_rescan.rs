use rcc_preprocessor::{
    lexer::{tokenize, TokenType},
    Preprocessor,
};
use std::path::PathBuf;

fn tokens(text: &str) -> Vec<String> {
    tokenize(text)
        .unwrap()
        .into_iter()
        .filter_map(|t| match t.token_type {
            TokenType::Whitespace(_)
            | TokenType::Newline
            | TokenType::Comment(_)
            | TokenType::Eof => None,
            _ => Some(t.text),
        })
        .collect()
}
fn check(source: &str, expected: &str) {
    let output = Preprocessor::new()
        .process(source, PathBuf::from("rescan.c"))
        .unwrap();
    assert_eq!(
        tokens(&output),
        tokens(expected),
        "source: {source}\noutput: {output}"
    );
}

#[test]
fn raw_arguments_are_independent_of_macro_hash_order() {
    // Each fresh map has an independent seed. This reproduced Lynx's SELECT
    // attribute table failure, which previously depended on that seed.
    for _ in 0..64 {
        check(
            "#define SELECT\n#define ATTR(name) #name, name##_value\nATTR(SELECT)",
            "\"SELECT\", SELECT_value",
        );
    }
}

#[test]
fn prescan_only_applies_to_ordinary_parameter_occurrences() {
    check(
        concat!(
            "#define BAD(a,b) a+b\n#define IGNORE(x) 7\n#define RAW(x) #x\n",
            "#define PASTE(x) x##_suffix\nIGNORE(BAD(1)) RAW(BAD(1)) PASTE(BAD)"
        ),
        "7 \"BAD(1)\" BAD_suffix",
    );
    check(
        "#define N 42\n#define BOTH(x) #x, x, x##_suffix\nBOTH(N)",
        "\"N\", 42, N_suffix",
    );
}

#[test]
fn nested_calls_aliases_and_rescan_across_replacement_boundaries() {
    check(
        "#define F(x) ((x)+1)\n#define ALIAS F\nALIAS(F(2))",
        "((((2)+1))+1)",
    );
    check("#define F() G\n#define G(x) x+x\nF()(3)", "3+3");
    check("#define PAIR a,b\n#define ID(x) x\nID(PAIR)", "a,b");
    check(
        "#define RAW(x) #x\n#define STR(x) RAW(x)\n#define A B\n#define B 8\nSTR(A)",
        "\"8\"",
    );
    check("#define F(x) expanded\n#define EMPTY\nF EMPTY (1)", "F (1)");
}

#[test]
fn direct_and_indirect_self_references_stop_without_losing_tokens() {
    check("#define X X+1\nX X", "X+1 X+1");
    check(
        "#define X (4+Y)\n#define Y (2*X)\nX Y",
        "(4+(2*X)) (2*(4+Y))",
    );
    check("#define f(x) x x\nf(f(1)) f(f)(2)", "1 1 1 1 f f(2)");
    check(
        "#define foo(x) bar x\n#define bar(x) foo x\nfoo(foo)(2)",
        "bar foo(2)",
    );
}

#[test]
fn pasted_arguments_use_boundary_tokens_and_empty_placemarkers() {
    check(
        "#define CAT(a,b) a##b\n#define abc 19\nCAT(ab,c) CAT(a b,c d) CAT(,) CAT(,x) CAT(x,)",
        "19 a bc d x x",
    );
    check(
        "#define JOIN(a,b,c) a##b##c\nJOIN(a,b,c) JOIN(a,,c) JOIN(,,)",
        "abc ac",
    );
    check("#define JOIN(a,b) a##b\nJOIN(+,=) JOIN(1.5,e3)", "+= 1.5e3");
    check("#define OBJECT a##b##c\nOBJECT", "abc");
}

#[test]
fn variadic_parameters_are_prescanned_or_kept_raw_as_required() {
    check(
        concat!(
            "#define N 7\n#define VA(...) __VA_ARGS__\n#define RAW(...) #__VA_ARGS__\n",
            "#define P(x,...) x##__VA_ARGS__\nVA(N,N) RAW(N, N) P(left,right) VA()"
        ),
        "7,7 \"N, N\" leftright",
    );
}

#[test]
fn stringizing_preserves_literal_whitespace_and_escapes() {
    check(
        r#"#define S(x) #x
S(  a   + /**/ b ) S("a  b\n" '\\')
"#,
        r#""a + b" "\"a  b\\n\" '\\\\'""#,
    );
}

#[test]
fn expansion_preserves_preprocessing_token_boundaries() {
    check("#define F(x) x+x\n#define EMPTY\n#define EXP e3\n#define PREFIX L\nF(+) +EMPTY+ 1EXP PREFIX\"x\"", "+ + + + + 1EXP L \"x\"");
    check("#define ID(x) x\n#define DOT .\nDOT.ID(.)", ". . .");
    check(
        "#define e 9\n#define xff 8\n1e+3 0xff .5e-2 L\"e\" 'e' \"e\"",
        "1e+3 0xff .5e-2 L\"e\" 'e' \"e\"",
    );
}

#[test]
fn builtins_participate_in_prescan_but_not_stringizing() {
    let mut pp = Preprocessor::new();
    pp.process(
        "#define S(x) #x\n#define STR(x) S(x)\n#define LINE __LINE__\n",
        PathBuf::from("rescan.c"),
    )
    .unwrap();
    let line = pp.expand_macros_impl("__LINE__").unwrap();
    let output = pp
        .expand_macros_impl("S(__LINE__) STR(LINE) S(__FILE__) STR(__FILE__)")
        .unwrap();
    assert_eq!(
        tokens(&output),
        tokens(&format!(r#""__LINE__" "{line}" "__FILE__" "\"rescan.c\"""#))
    );
}

#[test]
fn malformed_invocations_and_invalid_pastes_report_errors() {
    for source in [
        "#define F(a,b) a\nF(1)",
        "#define F(x) x\nF(1",
        "#define V(x,y,...) x\nV(1)",
        "#define CAT(a,b) a##b\nCAT(x,+)",
        "#define BAD(x) ##x\nBAD(1)",
        "#define BAD(x) x##\nBAD(1)",
    ] {
        assert!(
            Preprocessor::new()
                .process(source, PathBuf::from("bad.c"))
                .is_err(),
            "accepted {source}"
        );
    }
}

#[test]
fn c99_rescanning_example_preserves_disabled_macro_occurrences() {
    check(
        concat!(
            "#define x 3\n#define f(a) f(x * (a))\n#undef x\n#define x 2\n",
            "#define g f\n#define z z[0]\n#define h g(~\n#define m(a) a(w)\n",
            "#define w 0,1\n#define t(a) a\n",
            "f(y+1) + f(f(z)) % t(t(g)(0) + t)(1);\n",
            "g(x+(3,4)-w) | h 5) & m (f)^m(m);"
        ),
        concat!(
            "f(2 * (y+1)) + f(2 * (f(2 * (z[0])))) % f(2 * (0)) + t(1);\n",
            "f(2 * (2 +(3,4)-0,1)) | f(2 * (~ 5)) & f(2 * (0,1))^m(0,1);"
        ),
    );
}

#[test]
fn many_independent_calls_do_not_share_disabled_state() {
    let mut source = String::from("#define F(x) x+x\n");
    source.push_str(&"F(1) ".repeat(1000));
    check(&source, &"1+1 ".repeat(1000));
    let mut source = String::new();
    for i in 0..200 {
        source.push_str(&format!("#define A{i} A{}\n", i + 1));
    }
    source.push_str("#define A200 7\nA0");
    check(&source, "7");
}

#[test]
fn variadic_stringizing_retains_original_comma_whitespace() {
    check(
        "#define S(...) #__VA_ARGS__\nS(a,b) S(a , b) S(  a,  b  )",
        "\"a,b\" \"a , b\" \"a, b\"",
    );
    check(
        "#define S(x,...) #x #__VA_ARGS__\nS(ignored,a,b) S(ignored,a , b)",
        "\"ignored\" \"a,b\" \"ignored\" \"a , b\"",
    );
}

#[test]
fn extended_identifier_and_comment_boundaries_are_preserved() {
    check("#define caf 7\ncafé caf caf_suffix", "café 7 caf_suffix");
    let mut pp = Preprocessor::new();
    pp.set_keep_comments(true);
    let output = pp
        .process("#define N 7\n/* N */ N // N\n", PathBuf::from("comments.c"))
        .unwrap();
    assert!(output.contains("/* N */ 7 // N"), "{output}");
    check(
        "#define RAW(x) %:x\n#define CAT(a,b) a%:%:b\nRAW(unexpanded) CAT(ab,c)",
        "\"unexpanded\" abc",
    );
}
