#[test]
fn chained_collection_and_string_lengths_keep_their_usize_type() {
    rfl::check_source(include_str!("fixtures/console_lengths.rfl")).unwrap();
}

#[test]
fn length_hints_preserve_execution_and_user_defined_method_types() {
    let source = format!("{}\nstruct Number{{value:u32}} impl Number{{fn len(&self)->u32{{self.value}}}} fn check_number()->bool{{let number=Number{{value:70000}}; number.len()==70000}}",include_str!("fixtures/console_lengths.rfl").replace("println(\"lengths preserved\");","if(!check_number()){return 3;}println(\"lengths preserved\");"));
    for result in [
        rfl::run_native_program(&source).unwrap(),
        rfl::run_program(&source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "lengths preserved\n");
    }
    let error =
        rfl::check_source("fn values()->Vec<i16>{[1]} fn main(){let n=values().len()+(1 as i16);}")
            .unwrap_err();
    assert!(
        error.message.contains("usize") && error.message.contains("i16"),
        "{error}"
    );
}
#[test]
fn dereferenced_numbers_keep_their_pointee_width() {
    let source = include_str!("fixtures/deref-numeric-hints.rfl");
    rfl::check_source(source).unwrap();
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "true\n70002\n");
    }
}
#[test]
fn generic_calls_preserve_fixed_scalar_results_in_comparisons() {
    let source = include_str!("fixtures/generic-result-hints.rfl");
    rfl::check_source(source).unwrap();
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "true\n");
    }
}

#[test]
fn block_statements_do_not_consume_the_next_dereference_as_multiplication() {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures/block-before-deref.rfl");
    rfl::check_file(&path).unwrap();
    for result in [
        rfl::run_native_file(&path, false).unwrap(),
        rfl::run_file(&path).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "42\n");
    }
}

#[test]
fn boolean_equality_is_typed_and_ordering_and_arithmetic_remain_invalid() {
    let source = include_str!("fixtures/projected-bool-comparison.rfl");
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "true\ntrue\n");
    }
    for expr in ["true < false", "true + false", "true == 0"] {
        assert!(rfl::check_source(&format!("fn main(){{println({expr});}}")).is_err());
    }
}
