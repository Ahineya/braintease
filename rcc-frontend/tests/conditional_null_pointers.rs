use rcc_frontend::Frontend;

#[test]
fn conditional_null_constants_use_the_other_operands_pointer_type() {
    for null in [
        "0",
        "0L",
        "0UL",
        "0LL",
        "0ULL",
        "(2 - 2)",
        "(int)0",
        "(1 ? 0 : 0)",
        "ZERO",
        "(sizeof(int) - sizeof(int))",
    ] {
        for pointer in ["p", "a", "\"text\"", "fn", "&fn"] {
            for (left, right) in [(null, pointer), (pointer, null)] {
                let result_type = if pointer.contains("fn") {
                    "Callback"
                } else {
                    "char *"
                };
                let source = format!(
                    "enum {{ ZERO = 0 }};
                     typedef int (*Callback)(int);
                     int fn(int x) {{ return x; }}
                     {result_type} select(int c, char *p) {{
                         static char a[2];
                         return c ? {left} : {right};
                     }}"
                );
                Frontend::compile_to_ir(&source, "conditional_null.c")
                    .unwrap_or_else(|e| panic!("{left} : {right} must be pointer typed: {e}"));
            }
        }
    }
}

#[test]
fn conditional_pointer_rejects_non_null_integer_and_float_operands() {
    for non_null in ["1", "-1", "(2 - 1)", "zero", "(_Bool)1", "0.0"] {
        for (left, right) in [(non_null, "p"), ("p", non_null)] {
            // Using only sizeof ensures the conditional itself diagnoses the
            // invalid combination, rather than a surrounding pointer assignment.
            let source = format!(
                "int main(void) {{ int c = 1; int zero = 0; int *p;
                    return sizeof(c ? {left} : {right}); }}"
            );
            let error = Frontend::compile_to_ir(&source, "conditional_invalid.c")
                .expect_err("a runtime integer or nonzero constant is not a null pointer constant");
            assert!(
                error.to_string().contains("null pointer constant"),
                "{error}"
            );
        }
    }
}
