use super::*;

#[test]
fn structured_loop_parallel_phis_and_critical_edges() {
    let m = parse(
        r#"
function i16 @swap(i16 %0) {
 block 10:
 br label 20
 block 20:

 %1 = phi i16 [1, label 10], [%2, label 30]

 %2 = phi i16 [2, label 10], [%1, label 30]

 %3 = phi i16 [0, label 10], [%5, label 30]

 %4 = ult i1 %3, %0

 br %4, label 30, label 40
 block 30:

 %5 = add i16 %3, 1

 br label 20
 block 40:
 ret %1
}
function i16 @critical(i16 %0) {
 block 0:
 br %0, label 2, label 1
 block 1:
 br label 2
 block 2:

 %1 = phi i16 [11, label 0], [22, label 1]

 ret %1
}
"#,
    );
    let a = compile(&[m], &options(&["swap", "critical"])).unwrap();
    assert_eq!(a.statistics.structured_functions, 2);
    assert_eq!(a.statistics.dispatch_functions, 0);
    // Inspection guards the normal loop against accidental dispatcher lowering.
    for payload in wasmparser::Parser::new(0).parse_all(&a.bytes) {
        if let wasmparser::Payload::CodeSectionEntry(body) = payload.unwrap() {
            assert!(body
                .get_operators_reader()
                .unwrap()
                .into_iter()
                .all(|op| !matches!(op.unwrap(), wasmparser::Operator::BrTable { .. })));
        }
    }
    run(
        &a,
        vec![
            case("swap", &[0], 1),
            case("swap", &[1], 2),
            case("swap", &[200], 1),
            case("swap", &[201], 2),
            case("critical", &[0], 22),
            case("critical", &[1], 11),
        ],
    );
}

#[test]
fn nested_loops_self_loops_and_irreducible_fallback() {
    let m = parse(
        r#"
function i16 @nested(i16 %0) {
 block 0:
 br label 1
 block 1:

 %1 = phi i16 [0, label 0], [%7, label 5]

 %2 = phi i16 [0, label 0], [%5, label 5]

 %3 = ult i1 %1, %0

 br %3, label 2, label 6
 block 2:
 br label 3
 block 3:

 %4 = phi i16 [0, label 2], [%6, label 4]

 %5 = phi i16 [%2, label 2], [%8, label 4]

 %9 = ult i1 %4, 3

 br %9, label 4, label 5
 block 4:

 %6 = add i16 %4, 1

 %8 = add i16 %5, 1

 br label 3
 block 5:

 %7 = add i16 %1, 1

 br label 1
 block 6:
 ret %2
}
function i16 @selfloop() {
 block 0:
 br label 1
 block 1:

 %0 = phi i16 [0, label 0], [%1, label 1]

 %1 = add i16 %0, 1

 %2 = ult i1 %1, 5

 br %2, label 1, label 2
 block 2:
 ret %1
}
function i16 @irreducible(i16 %0) {
 block 0:
 br %0, label 1, label 2
 block 1:

 %1 = phi i16 [3, label 0], [%4, label 2]

 br label 2
 block 2:

 %2 = phi i16 [5, label 0], [%1, label 1]

 %3 = ne i1 %2, 0

 %4 = sub i16 %2, 1

 br %3, label 1, label 3
 block 3:
 ret %2
}
"#,
    );
    let a = compile(&[m], &options(&["nested", "selfloop", "irreducible"])).unwrap();
    assert_eq!(a.statistics.structured_functions, 2);
    assert_eq!(a.statistics.dispatch_functions, 1);
    run(
        &a,
        vec![
            case("nested", &[0], 0),
            case("nested", &[7], 21),
            case("selfloop", &[], 5),
            case("irreducible", &[0], 0),
            case("irreducible", &[1], 0),
        ],
    );
}
