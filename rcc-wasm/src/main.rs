//! Small IR linker/driver, usable before frontend runtime integration is ready.
use std::{fs, path::PathBuf};
fn run() -> Result<(), String> {
    let mut inputs = Vec::new();
    let mut exports = Vec::new();
    let mut output = PathBuf::from("a.wasm");
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-h" | "--help" => {
                println!("rcc-wasm INPUT.ir... [-o OUTPUT.wasm] [--export NAME]...\nDefaults to exporting main. Inputs must use target wasm.");
                return Ok(());
            }
            "-o" | "--output" => output = args.next().ok_or("missing output path")?.into(),
            "--export" => exports.push(args.next().ok_or("missing export name")?),
            _ if arg.starts_with('-') => return Err(format!("unknown option {arg}")),
            _ => inputs.push(PathBuf::from(arg)),
        }
    }
    if inputs.is_empty() {
        return Err("expected at least one input IR file; use --help".into());
    }
    let modules = inputs
        .iter()
        .map(|path| {
            let text = fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
            rcc_ir::parse_module(&text).map_err(|e| format!("{}: {e}", path.display()))
        })
        .collect::<Result<Vec<_>, String>>()?;
    let options = if exports.is_empty() {
        rcc_wasm::Options::default()
    } else {
        rcc_wasm::Options { exports }
    };
    let artifact = rcc_wasm::compile(&modules, &options)?;
    fs::write(&output, &artifact.bytes).map_err(|e| format!("{}: {e}", output.display()))?;
    eprintln!(
        "Wrote {} ({} bytes, {} structured / {} dispatch functions)",
        output.display(),
        artifact.bytes.len(),
        artifact.statistics.structured_functions,
        artifact.statistics.dispatch_functions
    );
    Ok(())
}
fn main() -> std::process::ExitCode {
    match run() {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("error: {e}");
            std::process::ExitCode::FAILURE
        }
    }
}
