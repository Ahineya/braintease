//! Companion imports share compiler/LSP resolution and can embed portable C.
#[test]
fn shared_ini_reader_runs_without_an_os_and_accepts_library_module_suffixes() {
    let root = std::env::temp_dir().join(format!("rfl-companions-{}", std::process::id()));
    std::fs::create_dir_all(&root).unwrap();
    let path = root.join("main.rfl");
    std::fs::write(&path, r#"
        import { parse, parse_bytes, parse_bytes_with_limits, Limits, InvalidSyntax, InputTooLarge } from "lib/ini.rfl";
        import {} from "std/result";
        fn main() -> i16 {
            let values = parse("[custom]\nkey=first\nkey='second'\n").unwrap();
            if (values.len() != 2 || values[1].section != "custom" || values[1].value != "second") { return 1; }
            let bytes: Vec<u8> = [91, 120, 93, 10, 107, 61, 118, 10];
            let raw = parse_bytes(bytes).unwrap();
            if (raw.len() != 1 || raw[0].section != "x" || raw[0].key != "k" || raw[0].value != "v") { return 4; }
            if (!parse_bytes([]).unwrap().is_empty()) { return 5; }
            let mut larger: Vec<u8> = [];
            for (i in 0..300) {
                larger.push(120); larger.push(61); larger.push(121); larger.push(10);
            }
            match parse_bytes(larger) { Err(InputTooLarge) => {}, _ => { return 7; } }
            let expanded = parse_bytes_with_limits(larger, Limits { input: 1200, line: 3, entries: 300 }).unwrap();
            if (expanded.len() != 300) { return 8; }
            match parse_bytes([91 as u8, 120, 93, 10, 107, 61, 0]) {
                Err(InvalidSyntax(2)) => {},
                _ => { return 6; }
            }
            match parse("[custom]\ninvalid\n") {
                Err(InvalidSyntax(line)) => if (line == 2) { 0 } else { 2 },
                _ => 3
            }
        }
    "#).unwrap();
    for result in [rfl::run_file(&path), rfl::run_native_file(&path, false)] {
        let result = result.unwrap();
        assert_eq!(result.status, 0, "{}", result.stdout);
    }
    for spec in ["lib/../std/io", "lib/", "lib//ini", "lib/./ini", "lib/ini.c", "lib/ini\\escape"] {
        std::fs::write(&path, format!("import {{}} from \"{}\"; fn main() {{}}", spec.replace('\\', "\\\\"))).unwrap();
        assert!(rfl::compile_file(&path).is_err(), "{spec}");
    }
    std::fs::remove_dir_all(root).unwrap();
}
