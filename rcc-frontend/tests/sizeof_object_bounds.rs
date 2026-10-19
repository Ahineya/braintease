use rcc_frontend::Frontend;
#[test]
fn object_size_bounds_and_array_alias_parameters() {
    Frontend::compile_to_ir(include_str!("../../c-test/tests/types/test_typedef_array_parameter.c"), "bounds.c").unwrap();
}
#[test]
fn incomplete_or_nonconstant_object_bounds_are_rejected() {
    for source in ["extern int values[]; int a[sizeof values];", "int count; int a[count];"] {
        assert!(Frontend::compile_to_ir(source, "bounds.c").is_err());
    }
}
