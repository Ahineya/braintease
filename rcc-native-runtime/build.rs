use std::{env, fs, path::PathBuf};

fn main() {
    if env::var_os("CARGO_FEATURE_HOST_TUI").is_none() {
        return;
    }
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("..");
    let include = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("host-tui-include");
    fs::create_dir_all(&include).unwrap();
    // Only these portable headers: never put the guest libc headers on a host
    // compiler's include path (their sizes and system structures differ).
    for (from, to) in [
        ("ports/fuzix/ripple/include/curses.h", "curses.h"),
        ("runtime/include/ripple_unicode.h", "ripple_unicode.h"),
    ] {
        println!("cargo:rerun-if-changed={}", root.join(from).display());
        fs::copy(root.join(from), include.join(to)).unwrap();
    }
    let upstream = root.join("ports/fuzix/upstream/Library/libs/curses");
    for directory in ["libraries/unicode", "libraries/ini", "libraries/tui-theme",
        "ports/fuzix/upstream/Library/libs/curses", "ports/fuzix/ripple/curses"] {
        println!("cargo:rerun-if-changed={}", root.join(directory).display());
    }
    let mut build = cc::Build::new();
    build
        .include(&include)
        .include(&upstream)
        .define("RIPPLE_CURSES_HOST", "1")
        .flag_if_supported("-std=c99");
    let omitted = [
        "initscr", "endwin", "newwin", "windel", "wgetch", "wgetstr", "doupdt", "refresh",
        "winscrol", "waddch", "waddstr",
    ];
    let mut sources: Vec<_> = fs::read_dir(&upstream)
        .unwrap()
        .map(|p| p.unwrap().path())
        .filter(|p| {
            p.extension().is_some_and(|e| e == "c")
                && !omitted.contains(&p.file_stem().unwrap().to_str().unwrap())
        })
        .collect();
    sources.extend(
        fs::read_dir(root.join("ports/fuzix/ripple/curses"))
            .unwrap()
            .map(|p| p.unwrap().path())
            .filter(|p| p.extension().is_some_and(|e| e == "c")),
    );
    sources.extend(
        [
            "libraries/unicode/unicode.c",
            "libraries/ini/ini.c",
            "libraries/tui-theme/tui_theme.c",
            "rfl/os/curses.c",
            "rfl/host/signals.c",
        ]
        .map(|p| root.join(p)),
    );
    sources.sort();
    for source in sources {
        println!("cargo:rerun-if-changed={}", source.display());
        build.file(source);
    }
    build.compile("rfl_host_tui");
    println!("cargo:rustc-link-lib=ncurses");
}
