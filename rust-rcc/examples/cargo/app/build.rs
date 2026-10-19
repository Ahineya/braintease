fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rustc-env=CELL_GENERATED=package-env-ok");
    let path = std::path::PathBuf::from(std::env::var_os("OUT_DIR").unwrap());
    std::fs::write(path.join("root.rs"), "const ROOT_NUMBER: u16 = 19;\n").unwrap();
}
