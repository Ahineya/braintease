fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    let path = std::path::PathBuf::from(std::env::var_os("OUT_DIR").unwrap());
    std::fs::write(
        path.join("generated.rs"),
        "pub const BUILD_OFFSET: u32 = 7;\n",
    )
    .unwrap();
}
