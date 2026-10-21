//! Build-time export of native gterm's profile-selected, antialiased glyphs.
#[cfg(target_os = "macos")]
#[allow(dead_code)]
#[path = "../hosted_terminal.rs"]
mod hosted_terminal;

#[cfg(target_os = "macos")]
#[link(name = "Foundation", kind = "framework")]
unsafe extern "C" {}

fn main() {
    if let Err(error) = run() {
        eprintln!("rcc-terminal-font: {error}");
        std::process::exit(1);
    }
}

#[cfg(target_os = "macos")]
fn run() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if args.len() != 2 {
        return Err("usage: rcc-terminal-font HEIGHT OUTPUT.atlas".into());
    }
    let height = args[0].to_str().ok_or("invalid height")?.parse()?;
    let atlas = hosted_terminal::boot_atlas(height)?;
    std::fs::write(&args[1], atlas)?;
    Ok(())
}

#[cfg(not(target_os = "macos"))]
fn run() -> Result<(), Box<dyn std::error::Error>> {
    Err(
        "native gterm font export currently requires macOS; use a prebuilt atlas or the ROM font"
            .into(),
    )
}
