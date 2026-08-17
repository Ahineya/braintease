#[cfg(test)]
mod tests {
    use super::super::*;
    use indoc::indoc;
    use std::path::PathBuf;

    fn preprocess(input: &str) -> Result<String> {
        let mut preprocessor = Preprocessor::new();
        preprocessor.process(input, PathBuf::from("test.c"))
    }

    fn preprocess_with_defines(input: &str, defines: Vec<(&str, &str)>) -> Result<String> {
        let mut preprocessor = Preprocessor::new();
        for (name, value) in defines {
            preprocessor.define(name.to_string(), Some(value.to_string()));
        }
        preprocessor.process(input, PathBuf::from("test.c"))
    }

    #[test]
    fn test_basic_passthrough() {
        let input = "int main() { return 0; }";
        let output = preprocess(input).unwrap();
        assert_eq!(output.trim(), input);
    }

    #[test]
    fn test_simple_define() {
        let input = indoc! {"
            #define MAX 100
            int array[MAX];
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("int array[100];"));
    }

    #[test]
    fn test_define_with_value() {
        let input = indoc! {"
            #define PI 3.14159
            double circle = PI * r * r;
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("3.14159 * r * r"));
    }

    #[test]
    fn test_function_like_macro() {
        let input = indoc! {"
            #define MIN(a, b) ((a) < (b) ? (a) : (b))
            int x = MIN(5, 10);
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("((5) < (10) ? (5) : (10))"));
    }

    #[test]
    fn test_nested_macros() {
        let input = indoc! {"
            #define X 1
            #define Y X + 2
            int z = Y;
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("1 + 2"));
    }

    #[test]
    fn test_undef() {
        let input = indoc! {"
            #define X 10
            int a = X;
            #undef X
            int b = X;
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("int a = 10;"));
        assert!(output.contains("int b = X;")); // X should not be expanded after undef
    }

    #[test]
    fn test_ifdef_defined() {
        let input = indoc! {"
            #define DEBUG
            #ifdef DEBUG
            int debug_var = 1;
            #endif
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("int debug_var = 1;"));
    }

    #[test]
    fn test_ifdef_undefined() {
        let input = indoc! {"
            #ifdef UNDEFINED
            int should_not_appear = 1;
            #endif
            int should_appear = 2;
        "};
        let output = preprocess(input).unwrap();
        assert!(!output.contains("should_not_appear"));
        assert!(output.contains("int should_appear = 2;"));
    }

    #[test]
    fn test_ifndef() {
        let input = indoc! {"
            #ifndef GUARD
            #define GUARD
            int guarded_var = 1;
            #endif
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("int guarded_var = 1;"));
    }

    #[test]
    fn test_else_clause() {
        let input = indoc! {"
            #ifdef UNDEFINED
            int a = 1;
            #else
            int b = 2;
            #endif
        "};
        let output = preprocess(input).unwrap();
        assert!(!output.contains("int a = 1;"));
        assert!(output.contains("int b = 2;"));
    }

    #[test]
    fn test_elif() {
        let input = indoc! {"
            #define VERSION 2
            #if VERSION == 1
            int v1 = 1;
            #elif VERSION == 2
            int v2 = 2;
            #else
            int v3 = 3;
            #endif
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("int v2 = 2;"));
        assert!(!output.contains("int v1 = 1;"));
        assert!(!output.contains("int v3 = 3;"));
    }

    #[test]
    fn test_nested_conditionals() {
        let input = indoc! {"
            #define A
            #define B
            #ifdef A
                #ifdef B
                int both = 1;
                #else
                int only_a = 2;
                #endif
            #else
            int neither = 3;
            #endif
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("int both = 1;"));
        assert!(!output.contains("int only_a = 2;"));
        assert!(!output.contains("int neither = 3;"));
    }

    #[test]
    fn test_multiline_macro() {
        let input = indoc! {r#"
            #define LONG_MACRO \
                int a = 1; \
                int b = 2;
            LONG_MACRO
        "#};
        let output = preprocess(input).unwrap();
        assert!(output.contains("int a = 1"));
        assert!(output.contains("int b = 2"));
    }

    #[test]
    fn test_variadic_macro() {
        let input = indoc! {"
            #define PRINTF(fmt, ...) printf(fmt, __VA_ARGS__)
            PRINTF(\"Hello %s\", \"world\");
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("printf(\"Hello %s\", \"world\")"));
    }

    #[test]
    fn test_token_pasting() {
        let input = indoc! {"
            #define CONCAT(a, b) a##b
            int CONCAT(var, 123) = 5;
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("int var123 = 5;"));
    }

    #[test]
    fn test_stringification() {
        let input = indoc! {"
            #define STR(x) #x
            char* s = STR(hello);
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("char* s = \"hello\";"));
    }

    #[test]
    fn test_error_directive() {
        let result = preprocess("#error boom\nint x;\n");
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("#error boom"));
    }

    #[test]
    fn test_error_skipped_in_false_if() {
        let input = indoc! {"
            #if 0
            #error should not fire
            #endif
            int ok = 1;
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("int ok = 1;"));
    }

    #[test]
    fn test_if_arithmetic() {
        let input = indoc! {"
            #if 1+1 == 2
            int sum = 1;
            #endif
            #if 1 && 0
            int both = 1;
            #endif
            #if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
            int c99 = 1;
            #endif
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("int sum = 1;"));
        assert!(!output.contains("int both = 1;"));
        assert!(output.contains("int c99 = 1;"));
    }

    #[test]
    fn test_file_line_builtins() {
        let input = "const char *f = __FILE__; int line = __LINE__;";
        let output = preprocess(input).unwrap();
        assert!(output.contains("\"test.c\""));
        assert!(output.contains("int line = "));
        assert!(!output.contains("__LINE__"));
    }

    #[test]
    fn test_comments_removed() {
        let input = indoc! {"
            // Line comment
            int a = 1;
            /* Block comment */
            int b = 2;
            /* Multi-line
               block comment */
            int c = 3;
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("int a = 1;"));
        assert!(output.contains("int b = 2;"));
        assert!(output.contains("int c = 3;"));
        // Comments should be removed by default
        assert!(!output.contains("Line comment"));
        assert!(!output.contains("Block comment"));
    }

    #[test]
    fn test_pragma_once() {
        // This would require file operations, so we test the concept
        let input = indoc! {"
            #pragma once
            int header_var = 1;
        "};
        let output = preprocess(input).unwrap();
        // Pragma once is processed internally and not output to the result
        // The code after it should still be present
        assert!(output.contains("int header_var = 1;"));
    }

    #[test]
    fn test_line_directive() {
        let input = indoc! {"
            #line 100 \"other.c\"
            int x = 1;
        "};
        let mut preprocessor = Preprocessor::new();
        preprocessor.set_keep_line_directives(true);
        let output = preprocessor.process(input, PathBuf::from("test.c")).unwrap();
        assert!(output.contains("#line 100 \"other.c\""));
    }

    #[test]
    fn test_defined_operator() {
        let input = indoc! {"
            #define FOO
            #if defined(FOO)
            int foo_defined = 1;
            #endif
            #if defined(BAR)
            int bar_defined = 1;
            #endif
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("int foo_defined = 1;"));
        assert!(!output.contains("int bar_defined = 1;"));
    }

    #[test]
    fn test_command_line_defines() {
        let input = "int x = DEBUG_LEVEL;";
        let output = preprocess_with_defines(input, vec![("DEBUG_LEVEL", "3")]).unwrap();
        assert!(output.contains("int x = 3;"));
    }

    #[test]
    fn test_macro_arguments_with_spaces() {
        let input = indoc! {"
            #define ADD(a, b) ((a) + (b))
            int sum = ADD( 1 , 2 );
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("((1) + (2))"));
    }

    #[test]
    fn test_macro_in_macro_args() {
        let input = indoc! {"
            #define X 5
            #define Y 10
            #define MAX(a, b) ((a) > (b) ? (a) : (b))
            int m = MAX(X, Y);
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("((5) > (10) ? (5) : (10))"));
    }

    #[test]
    fn test_empty_macro() {
        let input = indoc! {"
            #define EMPTY
            int EMPTY x EMPTY = EMPTY 1 EMPTY;
        "};
        let output = preprocess(input).unwrap();
        assert!(output.contains("int  x  =  1 ;"));
    }

    #[test]
    fn test_recursive_macro_protection() {
        let input = indoc! {"
            #define X X
            int x = X;
        "};
        // Current implementation will hit max recursion depth
        // This is expected behavior - it prevents infinite loops
        let result = preprocess(input);
        // The result might be an error due to max expansion depth
        // This is fine - we just want to make sure it doesn't panic
        // or cause a stack overflow
        let _ = result; // Don't check if it's ok or err, just that it completes
    }

    #[test]
    fn test_max_include_depth() {
        // This would require actual file includes
        // We can test that the depth limit is enforced
        let mut preprocessor = Preprocessor::new();
        preprocessor.include_depth = crate::MAX_INCLUDE_DEPTH;
        
        // Trying to include should fail
        let result = preprocessor.handle_include_impl("dummy.h".to_string(), false);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("Maximum include depth"));
    }

    #[test]
    fn test_multiline_function_macro() {
        let input = indoc! {r#"
            #define SWAP(a, b) do { \
                int temp = a; \
                a = b; \
                b = temp; \
            } while(0)
            SWAP(x, y);
        "#};
        let output = preprocess(input).unwrap();
        assert!(output.contains("do {"));
        assert!(output.contains("int temp = x;"));
        assert!(output.contains("x = y;"));
        assert!(output.contains("y = temp;"));
        assert!(output.contains("} while(0)"));
    }

    #[test]
    fn test_ifdef_else_endif_combinations() {
        // Test all combinations
        let tests = vec![
            ("#ifdef DEF\nyes\n#else\nno\n#endif", "no"),
            ("#ifndef DEF\nyes\n#else\nno\n#endif", "yes"),
            ("#ifdef DEF\nyes\n#endif\nafter", "after"),
            ("#ifndef DEF\nyes\n#endif\nafter", "yes"),
        ];
        
        for (input, expected) in tests {
            let output = preprocess(input).unwrap();
            assert!(output.contains(expected), 
                   "Failed for input: {}, got output: {}", input, output);
        }
    }

    #[test]
    fn test_define_with_comments() {
        let input = indoc! {"
            #define A 420 // This comment should not be in the macro
            #define B 100 /* Block comment should also be stripped */
            #define C 200 // Comment at end
            int x = A;
            int y = B;
            int z = C;
        "};
        let output = preprocess(input).unwrap();
        // The values should be expanded without comments
        assert!(output.contains("int x = 420;"));
        assert!(output.contains("int y = 100;"));
        assert!(output.contains("int z = 200;"));
        // Comments should not appear in the output
        assert!(!output.contains("This comment"));
        assert!(!output.contains("Block comment"));
        assert!(!output.contains("Comment at end"));
    }

    #[test]
    fn test_complex_conditional_nesting() {
        let input = indoc! {"
            #define A
            #ifdef A
                outer_a
                #ifdef B
                    inner_b
                #else
                    inner_not_b
                    #ifdef C
                        inner_inner_c
                    #endif
                #endif
                outer_a_end
            #else
                outer_not_a
            #endif
            always
        "};
        
        let output = preprocess(input).unwrap();
        assert!(output.contains("outer_a"));
        assert!(!output.contains("inner_b"));
        assert!(output.contains("inner_not_b"));
        assert!(!output.contains("inner_inner_c")); // C is not defined
        assert!(output.contains("outer_a_end"));
        assert!(!output.contains("outer_not_a"));
        assert!(output.contains("always"));
    }

    #[test]
    fn test_string_with_double_slash() {
        let input = r#"char* url = "http://example.com";"#;
        let output = preprocess(input).unwrap();
        assert_eq!(output.trim(), r#"char* url = "http://example.com";"#);
        assert!(output.contains("http://example.com"));
    }

    #[test]
    fn test_string_with_comment_like_content() {
        let input = r#"char* msg = "Use // for comments and /* */ for blocks";"#;
        let output = preprocess(input).unwrap();
        assert!(output.contains("// for comments"));
        assert!(output.contains("/* */ for blocks"));
    }

    #[test]
    fn test_string_with_hash() {
        let input = r##"char* channel = "#general";"##;
        let output = preprocess(input).unwrap();
        assert_eq!(output.trim(), r##"char* channel = "#general";"##);
        assert!(output.contains("#general"));
    }

    #[test]
    fn test_string_with_preprocessor_like_content() {
        let input = r##"char* directive = "#include <stdio.h>";"##;
        let output = preprocess(input).unwrap();
        assert!(output.contains("#include <stdio.h>"));
    }

    #[test]
    fn test_string_with_escape_sequences() {
        let input = r#"char* str = "Line 1\nLine 2\tTabbed\\";"#;
        let output = preprocess(input).unwrap();
        assert!(output.contains(r#""Line 1\nLine 2\tTabbed\\""#));
    }

    #[test]
    fn test_string_with_escaped_quotes() {
        let input = r#"char* quoted = "He said \"Hello\"";"#;
        let output = preprocess(input).unwrap();
        assert!(output.contains(r#""He said \"Hello\"""#));
    }

    #[test]
    fn test_char_literal_with_slash() {
        let input = "char c = '/';";
        let output = preprocess(input).unwrap();
        assert_eq!(output.trim(), "char c = '/';");
    }

    #[test]
    fn test_char_literal_with_hash() {
        let input = "char c = '#';";
        let output = preprocess(input).unwrap();
        assert_eq!(output.trim(), "char c = '#';");
    }

    #[test]
    fn test_mixed_strings_and_comments() {
        let input = indoc! {r#"
            // Real comment
            char* str1 = "Not // a comment";
            /* Real block comment */
            char* str2 = "Not /* a */ comment";
            char* str3 = "URL: http://test.com"; // Actual comment
        "#};
        let output = preprocess(input).unwrap();
        assert!(!output.contains("Real comment"));
        assert!(!output.contains("Real block comment"));
        assert!(!output.contains("Actual comment"));
        assert!(output.contains(r#""Not // a comment""#));
        assert!(output.contains(r#""Not /* a */ comment""#));
        assert!(output.contains("http://test.com"));
    }

    #[test]
    fn test_multiline_string_literal() {
        let input = indoc! {r#"
            char* multi = "Line 1\
            Line 2\
            Line 3";
        "#};
        let output = preprocess(input).unwrap();
        // Should preserve the multiline string literal
        assert!(output.contains("Line 1"));
    }

    #[test]
    fn test_adjacent_string_literals() {
        let input = r#"char* str = "Part 1" " Part 2" " Part 3";"#;
        let output = preprocess(input).unwrap();
        // The preprocessor should preserve the string literals as-is
        assert_eq!(output.trim(), r#"char* str = "Part 1" " Part 2" " Part 3";"#);
    }

    #[test]
    fn test_string_in_macro() {
        let input = indoc! {r#"
            #define URL "http://example.com"
            char* site = URL;
        "#};
        let output = preprocess(input).unwrap();
        assert!(output.contains(r#""http://example.com""#));
    }

    #[test]
    fn test_string_with_macro_like_content() {
        let input = indoc! {r#"
            #define MSG "Use MAX_SIZE for limit"
            char* help = MSG;
        "#};
        let output = preprocess(input).unwrap();
        // MAX_SIZE inside the string should not be expanded
        assert!(output.contains("Use MAX_SIZE for limit"));
    }

    #[test]
    fn test_complex_escape_in_string() {
        let input = r#"char* complex = "Tab:\t Quote:\" Backslash:\\ Newline:\n";"#;
        let output = preprocess(input).unwrap();
        assert!(output.contains(r#"Tab:\t Quote:\" Backslash:\\ Newline:\n"#));
    }
}