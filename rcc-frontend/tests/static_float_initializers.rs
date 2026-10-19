use rcc_frontend::{ir::Value, Frontend};

#[test]
fn floating_static_initializers_convert_integer_and_float_elements() {
    let module = Frontend::compile_to_ir(
        r#"
        const float scalar = 3;
        const float table[] = {1, -2, .5, 3UL};
        double wide[] = {1, -2.0f, 65535U};
        struct Pair { float a; double b; } pair = {7, .25f};
        union First { float value; long bits; } first = {1};
    "#,
        "static_float_initializers.c",
    )
    .unwrap();
    let f = |value: f32| Value::Constant(value.to_bits() as i64);
    let d = |value: f64| Value::Constant(value.to_bits() as i64);
    for (name, expected) in [
        ("scalar", f(3.0)),
        (
            "table",
            Value::ConstantAggregate(vec![f(1.0), f(-2.0), f(0.5), f(3.0)]),
        ),
        (
            "wide",
            Value::ConstantAggregate(vec![d(1.0), d(-2.0), d(65535.0)]),
        ),
        ("pair", Value::ConstantAggregate(vec![f(7.0), d(0.25)])),
        ("first", Value::ConstantAggregate(vec![f(1.0)])),
    ] {
        let actual = module.globals.iter().find(|g| g.name == name).unwrap();
        assert_eq!(actual.initializer.as_ref(), Some(&expected), "{name}");
    }
}
