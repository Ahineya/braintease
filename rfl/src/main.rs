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
    Check { input: PathBuf },
    /// Compile, link, and run a source file on rvm
    Run {
        input: PathBuf,
        /// Execute the guest with rvm's Cranelift JIT
        #[arg(long)]
        jit: bool,
    },
    /// Start the language server on standard input and output
    Lsp,
    /// Compile a source file to Ripple IR or assembly
    Compile {
        input: PathBuf,
        /// Write assembly here. Defaults to the input path with a `.asm` extension.
        #[arg(short, long)]
        output: Option<PathBuf>,
        /// Print textual IR and do not write assembly
        #[arg(long)]
        emit_ir: bool,
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
        Command::Run { input, jit } => rfl::run_file_in_terminal(&input, jit),
        Command::Check { input } => {
            eprintln!("    Checking {}", input.display());
            rfl::compile_file(&input)?;
            eprintln!("    Finished");
            Ok(0)
        }
        Command::Compile {
            input,
            output,
            emit_ir,
        } => {
            eprintln!("   Compiling {}", input.display());
            if emit_ir {
                print!("{}", rfl::compile_file_to_ir_text(&input)?);
                eprintln!("    Finished");
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
