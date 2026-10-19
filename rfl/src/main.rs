use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand, ValueEnum};

/// VM engines share Ripple IR; native and Wasm have distinct code targets.
#[derive(Clone, Copy, Debug, PartialEq, Eq, ValueEnum)]
enum RunTarget {
    Ripple,
    Rbf,
    Native,
    Wasm,
}

#[derive(Parser)]
#[command(name = "rfl", about = "Ripple Functional Language compiler")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Build an application for Riplix, Fuse-X, or the macOS host
    Build {
        input: PathBuf,
        #[arg(long, value_parser = ["fuzix", "fuse-x", "macos"])]
        os: String,
        #[arg(short, long)]
        output: Option<PathBuf>,
        /// Operating-system SDK build/cache directory
        #[arg(long)]
        build_dir: Option<PathBuf>,
        /// C compiler used for the guest libc and final target lowering
        #[arg(long)]
        compiler: Option<PathBuf>,
    },
    /// Format RFL files in place, or read stdin and write formatted code to stdout
    Fmt {
        input: Vec<PathBuf>,
        /// Report files that need formatting without changing them
        #[arg(long)]
        check: bool,
    },
    /// Typecheck a source file
    Check {
        input: PathBuf,
        #[arg(long, default_value="ripple", value_parser=rfl::Target::parse)]
        target: rfl::Target,
    },
    /// Compile, link, and run a source file
    Run {
        input: PathBuf,
        /// Use the selected VM's JIT (rvm or rbf)
        #[arg(long)]
        jit: bool,
        /// Execution target; rbf runs Brainfuck with lightweight progress telemetry
        #[arg(long, default_value = "ripple", value_enum)]
        target: RunTarget,
    },
    /// Start the language server on standard input and output
    Lsp,
    /// Compile a source file to IR, Ripple assembly, a native executable, or Wasm
    Compile {
        input: PathBuf,
        /// Output path. Defaults to `.asm`, `.native`, or `.wasm` for the target.
        #[arg(short, long)]
        output: Option<PathBuf>,
        /// Print textual IR instead of generating target code
        #[arg(long)]
        emit_ir: bool,
        #[arg(long, default_value="ripple", value_parser=rfl::Target::parse)]
        target: rfl::Target,
    },
}

fn main() -> ExitCode {
    match run() {
        Ok(status) => ExitCode::from(status as u8),
        Err(err) => {
            eprintln!("{err}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<i32, rfl::CompileError> {
    let cli = Cli::parse();
    match cli.command {
        Command::Build {
            input,
            os,
            output,
            build_dir,
            compiler,
        } => {
            let output = output.unwrap_or_else(|| {
                input.with_extension(match os.as_str() {
                    "fuzix" => "rxe",
                    "macos" => "native",
                    _ => "fxe",
                })
            });
            let mut command = std::process::Command::new("python3");
            command
                .arg(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tools/build_os.py"))
                .arg(input)
                .args(["--os", &os])
                .arg("-o")
                .arg(output)
                .arg("--rfl")
                .arg(
                    std::env::current_exe()
                        .map_err(|err| rfl::CompileError::plain(err.to_string()))?,
                );
            if let Some(build) = build_dir {
                command.arg("--build-dir").arg(build);
            }
            if let Some(compiler) = compiler {
                command.arg("--rcc").arg(compiler);
            }
            let status = command.status().map_err(|err| {
                rfl::CompileError::plain(format!("cannot build OS application: {err}"))
            })?;
            Ok(status.code().unwrap_or(1))
        }
        Command::Fmt { input, check } => format_files(&input, check),
        Command::Lsp => {
            rfl::serve_lsp();
            Ok(0)
        }
        Command::Run { input, jit, target } => {
            if target == RunTarget::Wasm {
                if jit {
                    return Err(rfl::CompileError::plain(
                        "--jit selects a VM JIT; Wasm runs in the host WebAssembly engine",
                    ));
                }
                rfl::run_wasm_file(&input, true).map(|result| result.status)
            } else if target == RunTarget::Native {
                if jit {
                    return Err(rfl::CompileError::plain(
                        "--jit selects a VM JIT; native programs are ahead-of-time compiled",
                    ));
                }
                rfl::run_native_file(&input, true).map(|result| result.status)
            } else if target == RunTarget::Rbf {
                rfl::run_file_in_brainfuck(&input, jit)
            } else {
                rfl::run_file_in_terminal(&input, jit)
            }
        }
        Command::Check { input, target } => {
            eprintln!("    Checking {}", input.display());
            rfl::check_file_for_target(&input, target)?;
            eprintln!("    Finished");
            Ok(0)
        }
        Command::Compile {
            input,
            output,
            emit_ir,
            target,
        } => {
            eprintln!("   Compiling {}", input.display());
            if emit_ir {
                print!(
                    "{}",
                    rcc_ir::emit_module(&rfl::compile_file_for_target(&input, target)?)
                );
                eprintln!("    Finished");
                return Ok(0);
            }
            if target == rfl::Target::Native {
                let output = output.unwrap_or_else(|| input.with_extension("native"));
                rfl::compile_file_to_native(&input, &output)?;
                eprintln!("    Finished {}", output.display());
                return Ok(0);
            }
            if target == rfl::Target::Wasm {
                let output = output.unwrap_or_else(|| input.with_extension("wasm"));
                rfl::compile_file_to_wasm(&input, &output)?;
                eprintln!("    Finished {}", output.display());
                return Ok(0);
            }
            let asm = rfl::compile_file_to_asm(&input)?;
            eprintln!("    Finished");
            let output = output.unwrap_or_else(|| {
                let mut path = input;
                path.set_extension("asm");
                path
            });
            std::fs::write(&output, asm).map_err(|err| {
                rfl::CompileError::plain(format!("cannot write {}: {err}", output.display()))
            })?;
            Ok(0)
        }
    }
}

fn format_files(paths: &[PathBuf], check: bool) -> Result<i32, rfl::CompileError> {
    use std::io::{Read, Write};
    if paths.is_empty() {
        let mut source = String::new();
        std::io::stdin()
            .read_to_string(&mut source)
            .map_err(|err| rfl::CompileError::plain(format!("cannot read stdin: {err}")))?;
        let formatted = rfl::format_source(&source)?;
        if check {
            return Ok(i32::from(source != formatted));
        }
        std::io::stdout()
            .write_all(formatted.as_bytes())
            .map_err(|err| rfl::CompileError::plain(format!("cannot write stdout: {err}")))?;
        return Ok(0);
    }
    // Validate every input before overwriting any file.
    let mut changes = Vec::new();
    for path in paths {
        let source = std::fs::read_to_string(path).map_err(|err| {
            rfl::CompileError::plain(format!("cannot read {}: {err}", path.display()))
        })?;
        let formatted = rfl::format_source(&source).map_err(|err| {
            rfl::CompileError::plain(format!("{}: {}", path.display(), err.message))
        })?;
        if source != formatted {
            changes.push((path, formatted));
        }
    }
    for (path, formatted) in &changes {
        if check {
            eprintln!("Would format {}", path.display());
        } else {
            std::fs::write(path, formatted).map_err(|err| {
                rfl::CompileError::plain(format!("cannot write {}: {err}", path.display()))
            })?;
        }
    }
    Ok(i32::from(check && !changes.is_empty()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn run_accepts_flags_before_or_after_the_filename() {
        for args in [
            ["rfl", "run", "--jit", "examples/beep.rfl"],
            ["rfl", "run", "examples/beep.rfl", "--jit"],
        ] {
            let cli = Cli::try_parse_from(args).unwrap();
            let Command::Run { input, jit, .. } = cli.command else {
                panic!("expected the run command");
            };
            assert_eq!(input, PathBuf::from("examples/beep.rfl"));
            assert!(jit);
        }
    }

    #[test]
    fn brainfuck_target_accepts_flags_on_either_side_of_the_filename() {
        for args in [
            vec![
                "rfl",
                "run",
                "--target",
                "rbf",
                "examples/comprehensions.rfl",
            ],
            vec![
                "rfl",
                "run",
                "examples/comprehensions.rfl",
                "--target",
                "rbf",
            ],
            vec![
                "rfl",
                "run",
                "--jit",
                "examples/comprehensions.rfl",
                "--target",
                "rbf",
            ],
        ] {
            let cli = Cli::try_parse_from(&args).unwrap();
            let Command::Run { input, target, jit } = cli.command else {
                panic!("expected the run command");
            };
            assert_eq!(target, RunTarget::Rbf);
            assert_eq!(input, PathBuf::from("examples/comprehensions.rfl"));
            assert_eq!(jit, args.contains(&"--jit"));
        }
        // rbf selects an execution engine, not a different source/IR data model.
        assert!(Cli::try_parse_from(["rfl", "check", "main.rfl", "--target", "rbf"]).is_err());
        assert!(Cli::try_parse_from(["rfl", "compile", "main.rfl", "--target", "rbf"]).is_err());
    }
}
