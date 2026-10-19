#[test]
fn reusable_selection_input_and_file_panel_run_natively() {
    let path =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/tui_widgets.rfl");
    let result = rfl::run_native_file(&path, false).unwrap_or_else(|error| panic!("{error}"));
    assert_eq!(result.status, 0, "{}", result.stdout);
    assert!(
        result.stdout.contains(" Alpha.txt\n42\n>z.txt\n42\n"),
        "{}",
        result.stdout
    );
    assert!(
        result.stdout.contains("TUI_WIDGETS_OK"),
        "{}",
        result.stdout
    );
    assert!(
        result.stdout.contains("CACHE_STABLE\nCACHE_MOVE\n"),
        "{}",
        result.stdout
    );
    let moved = result
        .stdout
        .split("CACHE_MOVE\n")
        .nth(1)
        .unwrap()
        .split("CACHE_LISTING\n")
        .next()
        .unwrap();
    assert!(
        moved.contains(">Alpha.txt") && moved.contains(" z.txt") && !moved.contains("Name"),
        "{moved}"
    );
    let listing = result
        .stdout
        .split("CACHE_LISTING\n")
        .nth(1)
        .unwrap()
        .split("CACHE_DONE\n")
        .next()
        .unwrap();
    assert!(
        listing.contains("Name") && listing.contains(">Beta.txt"),
        "{listing}"
    );
}
