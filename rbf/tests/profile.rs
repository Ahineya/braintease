use std::{
    fs,
    path::PathBuf,
    process::Command,
    sync::atomic::{AtomicU64, Ordering},
};

const HEADER: &str = "BFHOST 1 BANK_SIZE 64 MEMORY_BANKS 4 INSTRUCTIONS 1 ENTRY 0 ORIGIN 8 TAPE_CELLS 2312 END_BFHOST\n";
static NEXT_DIRECTORY: AtomicU64 = AtomicU64::new(0);

fn directory() -> PathBuf {
    let path = std::env::temp_dir().join(format!(
        "rbf-profile-{}-{}",
        std::process::id(),
        NEXT_DIRECTORY.fetch_add(1, Ordering::Relaxed)
    ));
    fs::create_dir_all(&path).unwrap();
    path
}

#[test]
fn cli_writes_profile_on_execution_limit_without_polluting_stdout() {
    let directory = directory();
    let source = directory.join("loop.bf");
    let profile = directory.join("profile.json");
    let report = directory.join("run.json");
    // Required transport hooks are unreachable after the deliberately endless loop.
    fs::write(&source, "BFHOST 1 BANK_SIZE 64 MEMORY_BANKS 4 INSTRUCTIONS 1 ENTRY 0 ORIGIN 8 TAPE_CELLS 2312 END_BFHOST\n+[]$?!").unwrap();
    let result = Command::new(env!("CARGO_BIN_EXE_rbf"))
        .arg(&source)
        .args([
            "--headless",
            "--timeout",
            "5",
            "--max-dispatches",
            "17",
            "--profile",
        ])
        .arg(&profile)
        .arg("--report")
        .arg(&report)
        .output()
        .unwrap();
    assert!(!result.status.success());
    assert!(result.stdout.is_empty());
    assert!(
        profile.exists(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let diagnostics: serde_json::Value =
        serde_json::from_slice(&fs::read(&profile).unwrap()).unwrap();
    let run: serde_json::Value = serde_json::from_slice(&fs::read(&report).unwrap()).unwrap();
    assert_eq!(diagnostics["success"], false);
    assert_eq!(diagnostics["profiled_dispatches"], 17);
    assert_eq!(diagnostics["bf_dispatches"], 17);
    assert_eq!(diagnostics["stop_reason"], run["stop_reason"]);
    assert_eq!(diagnostics["source_sha256"], run["source_sha256"]);
    assert!(diagnostics["operations"]
        .as_array()
        .unwrap()
        .iter()
        .any(|operation| operation["kind"] == "literal_close"));
    fs::remove_dir_all(&directory).unwrap();
}

#[test]
fn zero_timeout_writes_profile_before_the_first_dispatch() {
    let directory = directory();
    let source = directory.join("loop.bf");
    let profile = directory.join("profile.json");
    fs::write(&source, format!("{HEADER}+[]$?!")).unwrap();
    let result = Command::new(env!("CARGO_BIN_EXE_rbf"))
        .arg(&source)
        .args(["--headless", "--timeout", "0", "--profile"])
        .arg(&profile)
        .output()
        .unwrap();
    assert!(!result.status.success());
    assert!(result.stdout.is_empty());
    let diagnostics: serde_json::Value =
        serde_json::from_slice(&fs::read(&profile).unwrap()).unwrap();
    assert_eq!(diagnostics["success"], false);
    assert_eq!(diagnostics["stop_reason"], "Wall-time limit exceeded");
    assert_eq!(diagnostics["profiled_dispatches"], 0);
    assert_eq!(diagnostics["bf_dispatches"], 0);
    assert_eq!(diagnostics["ripple_instructions"], 0);
    assert!(diagnostics["operations"].as_array().unwrap().is_empty());
    assert!(diagnostics["sites"].as_array().unwrap().is_empty());
    fs::remove_dir_all(directory).unwrap();
}

#[test]
fn loop_source_offsets_include_header_and_leading_whitespace() {
    let directory = directory();
    let source = directory.join("walk.bf");
    let profile = directory.join("profile.json");
    // Layout parsing trims leading whitespace, but offsets identify bytes in
    // the original file. Stop after the walk, before the required CPU hooks.
    let text = format!(" \n\t{HEADER}+[->]$?!");
    fs::write(&source, &text).unwrap();
    let result = Command::new(env!("CARGO_BIN_EXE_rbf"))
        .arg(&source)
        .args([
            "--headless",
            "--timeout",
            "5",
            "--max-dispatches",
            "2",
            "--profile",
        ])
        .arg(&profile)
        .output()
        .unwrap();
    assert!(!result.status.success());
    assert!(result.stdout.is_empty());
    let diagnostics: serde_json::Value =
        serde_json::from_slice(&fs::read(&profile).unwrap()).unwrap();
    assert_eq!(diagnostics["profiled_dispatches"], 2);
    let sites = diagnostics["sites"].as_array().unwrap();
    assert_eq!(sites.len(), 1);
    assert_eq!(sites[0]["kind"], "walk");
    assert_eq!(sites[0]["attempts"], 1);
    assert_eq!(sites[0]["completed"], 1);
    assert_eq!(sites[0]["fallbacks"], 0);
    assert_eq!(sites[0]["source_offset"], text.find("[->]").unwrap());
    fs::remove_dir_all(directory).unwrap();
}
