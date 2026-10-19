fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=RUSTC");
    let output = std::process::Command::new(std::env::var_os("RUSTC").unwrap())
        .args(["--print", "sysroot"])
        .output()
        .unwrap();
    assert!(output.status.success());
    let sysroot = String::from_utf8(output.stdout).unwrap();
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}/lib", sysroot.trim());
}
