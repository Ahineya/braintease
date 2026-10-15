//! Bundle the small privileged macOS Ethernet helper into every runtime.
use std::{env, path::PathBuf, process::Command};

fn main() {
    println!("cargo:rerun-if-changed=../tools/network/vmnet.c");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") {
        return;
    }
    let sdk = Command::new("xcrun")
        .args(["--sdk", "macosx", "--show-sdk-path"])
        .output()
        .expect("locate macOS SDK for the Ethernet helper");
    assert!(
        sdk.status.success(),
        "xcrun: {}",
        String::from_utf8_lossy(&sdk.stderr)
    );
    let architecture = match env::var("CARGO_CFG_TARGET_ARCH").unwrap().as_str() {
        "aarch64" => "arm64",
        "x86_64" => "x86_64",
        other => panic!("unsupported macOS helper architecture: {other}"),
    };
    let output = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("ripple-net-vmnet");
    let status = Command::new("xcrun")
        .args([
            "clang",
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fblocks",
            "-arch",
            architecture,
            "-isysroot",
        ])
        .arg(String::from_utf8(sdk.stdout).unwrap().trim())
        .args(["../tools/network/vmnet.c", "-framework", "vmnet", "-o"])
        .arg(output)
        .status()
        .expect("compile macOS Ethernet helper");
    assert!(status.success(), "macOS Ethernet helper compilation failed");
}
