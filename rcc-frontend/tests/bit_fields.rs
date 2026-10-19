use rcc_frontend::Frontend;

#[test]
fn packed_bit_fields_compile_with_union_aliasing_and_initializers() {
    Frontend::compile_to_ir(include_str!("../../c-test/tests/types/test_bit_fields.c"), "bit_fields.c").unwrap();
}

#[test]
fn invalid_bit_fields_are_diagnosed() {
    for declaration in ["unsigned x:17;", "int x:-1;", "int x:0;", "float x:1;", "_Bool x:2;", "int x:n;"] {
        assert!(Frontend::compile_to_ir(&format!("int n; struct S {{ {declaration} }};"), "bad.c").is_err(), "{declaration}");
    }
    for expression in ["&s.a", "sizeof s.a"] {
        assert!(Frontend::compile_to_ir(&format!("struct S {{ unsigned a:3; }}; int main(void) {{ struct S s; {expression}; }}"), "bad.c").is_err(), "{expression}");
    }
}
