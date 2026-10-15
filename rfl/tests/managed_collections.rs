fn run(source: &str) -> rfl::RunResult {
    rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"))
}

#[test]
fn arrays_and_slices_own_strings_after_return_and_replacement() {
    let result = run(r#"
        fn view() -> Slice<String> {
            let fixed: [String; 2] = ["left", "right"];
            fixed[0..2]
        }
        fn main() -> i16 {
            let mut a: [String; 2] = ["a", "b"];
            let b = a;
            a[0] = "changed";
            a = ["new", "array"];
            let v = a.to_vec();
            let s = view();
            if (b[0] == "a" && v[1] == "array" && s[1] == "right") { 42 } else { 1 }
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn nested_vectors_and_struct_elements_detach_without_destroying_aliases() {
    let result = run(r#"
        struct Row { title: String, cells: Vec<String> }
        fn main() -> i16 {
            let mut rows = [Row { title: "row", cells: ["a", "b"] }];
            let original = rows;
            let mut row = rows[0];
            row.cells[0] = "x";
            rows[0] = row;
            rows.push(Row { title: "second", cells: ["c"] });
            let nested = [["keep"], ["also"]];
            let mut changed = nested;
            changed[0] = ["new"];
            rows.clear();
            if (original[0].cells[0] == "a" && nested[0][0] == "keep" && changed[0][0] == "new") { 42 } else { 1 }
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn managed_get_pop_and_growth_preserve_adt_and_closure_elements() {
    let result = run(r#"
        type Entry = | Text(String) | Empty;
        fn make() -> Vec<fn(i16) -> i16> {
            let offset = 40;
            [fn(n: i16) => n + offset]
        }
        fn main() -> i16 {
            let mut entries: Vec<Entry> = [];
            for (_ in 0..12) { entries.push(Text("owned")); }
            let saved = entries.get(0);
            let popped = entries.pop();
            entries.clear();
            let functions = make();
            let f = functions[0];
            match saved {
                Some(Text(s)) => if (s == "owned") {
                    match popped { Some(Text(t)) => if (t == "owned") { f(2) } else { 1 }, _ => 2 }
                } else { 3 },
                _ => 4,
            }
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn aggregate_elements_and_empty_operations_survive_repeated_reuse() {
    let result = run(r#"
        struct Boxed { values: [String; 2] }
        fn main() -> i16 {
            let mut items: Vec<Boxed> = [];
            let mut i = 0;
            while (i < 80) {
                items.push(Boxed { values: ["one", "two"] });
                let copy = items;
                items[0] = Boxed { values: ["three", "four"] };
                items.clear();
                if (copy[0].values[1] != "two") { return 1; }
                i += 1;
            }
            match items.pop() { None => match items.get(0) { None => 42, Some(_) => 2 }, Some(_) => 3 }
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn indexing_keeps_receiver_snapshot_when_index_mutates_it() {
    let result = run(r#"
        fn change(xs: &mut Vec<String>) -> usize { xs[0] = "new"; 0 }
        fn main() -> i16 {
            let mut xs = ["old"];
            let prior = xs[change(&mut xs)];
            if (prior == "old" && xs[0] == "new") { 42 } else { 1 }
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn collections_reject_borrowed_elements_even_inside_tuples() {
    for source in [
        "fn main() { let x = 1; let xs = [&x]; }",
        "fn main() { let x = 1; let xs = [(&x, 2)]; }",
    ] {
        let err = rfl::check_source(source).unwrap_err();
        assert!(err.message.contains("reference"), "{err}");
    }
}

#[test]
fn unused_managed_types_do_not_require_heap_support() {
    let result = run(r#"
        fn unused(f: fn() -> i16) -> Vec<Option<fn() -> i16>> { [Some(f)] }
        fn main() -> i16 { 42 }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn too_many_element_owners_fail_before_reference_counts_wrap() {
    let result = run(r#"
        fn main() {
            let text = "shared";
            let mut owners: Vec<String> = [];
            for (_ in 0..66000) { owners.push(text); }
        }
    "#);
    assert_eq!(result.status, 1, "{}", result.stdout);
    assert!(result.stdout.contains("overflow"), "{}", result.stdout);
}
