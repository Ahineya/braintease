use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(name = "rfl", about = "Ripple Functional Language compiler")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Typecheck a source file
    Check {
        input: PathBuf,
        #[arg(long, default_value="ripple", value_parser=rfl::Target::parse)]
        target: rfl::Target,
    },
    /// Compile, link, and run a source file
    Run {
        input: PathBuf,
        /// Execute the guest with rvm's Cranelift JIT
        #[arg(long)]
        jit: bool,
        /// Code generation target (native v1 retains the Ripple data ABI)
        #[arg(long, default_value="ripple", value_parser=rfl::Target::parse)]
        target: rfl::Target,
    },
    /// Start the language server on standard input and output
    Lsp,
    /// Compile a source file to IR, Ripple assembly, or a native executable
    Compile {
        input: PathBuf,
        /// Output path. Defaults to `.asm` for Ripple or `.native` for native.
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
        Command::Lsp => {
            rfl::serve_lsp();
            Ok(0)
        }
        Command::Run { input, jit, target } => {
            if target == rfl::Target::Native {
                if jit {
                    return Err(rfl::CompileError::plain("--jit selects the Ripple VM JIT; native programs are ahead-of-time compiled"));
                }
                rfl::run_native_file(&input, true).map(|result| result.status)
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
}
