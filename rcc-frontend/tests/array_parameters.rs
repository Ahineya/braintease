use rcc_frontend::{Frontend, TopLevelItem, Type};

#[test]
fn array_parameter_bounds_adjust_to_pointers() {
    for declarator in [
        "a[static 9]", "a[const static 9]", "a[static restrict 9]",
        "a[restrict 9]", "[static 9]", "(a)[static 9]",
    ] {
        let source = format!("void f(double {declarator});");
        let ast = Frontend::parse_source(&source).unwrap_or_else(|e| panic!("{source}: {e}"));
        let TopLevelItem::Declarations(decls) = &ast.items[0] else { panic!() };
        let Type::Function { parameters, .. } = &decls[0].decl_type else { panic!() };
        assert!(matches!(&parameters[0], Type::Pointer { target, .. } if **target == Type::Double), "{source}");
    }
}

#[test]
fn bracket_volatile_qualifies_the_adjusted_pointer() {
    let ast = Frontend::parse_source("void f(int a[static volatile 3], volatile int b[3]);").unwrap();
    let TopLevelItem::Declarations(decls) = &ast.items[0] else { panic!() };
    let Type::Function { parameters, .. } = &decls[0].decl_type else { panic!() };
    let Type::Volatile(pointer) = &parameters[0] else { panic!("missing pointer qualifier") };
    assert!(matches!(pointer.as_ref(), Type::Pointer { target, .. } if **target == Type::Int));
    assert!(matches!(&parameters[1], Type::Pointer { target, .. } if matches!(target.as_ref(), Type::Volatile(_))));
}

#[test]
fn nested_callback_array_parameters_keep_their_own_context() {
    Frontend::parse_source("void f(int (*callbacks[static 2])(int [static 3]));").unwrap();
    Frontend::parse_source("int f(int a[static 3]) { return a[0]; }").unwrap();
}

#[test]
fn invalid_array_contract_locations_and_syntax_are_rejected() {
    for source in [
        "int a[static 3];", "struct S { int a[static 3]; };",
        "void f(void) { int a[static 3]; }", "void f(int a[static]);",
        "void f(int a[static 0]);", "void f(int a[static -1]);",
        "void f(int a[static static 3]);", "void f(int a[const static volatile 3]);",
        "void f(int a[3][static 2]);", "void f(int (*a)[static 3]);",
        "void f(int a[sizeof(int [static 3])]);",
        "void f(void (*cb)(struct S { int a[static 3]; }));",
        // Dynamic parameter bounds already lack an entry-time evaluation
        // representation. Keep rejecting them instead of dropping side effects.
        "void f(int n, int a[static n++]);",
    ] {
        assert!(Frontend::parse_source(source).is_err(), "accepted invalid/unsupported declaration: {source}");
    }
}
