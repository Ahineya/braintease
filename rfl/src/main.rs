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
    Run { input: PathBuf },
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
            eprintln!("error: {err}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<i32, rfl::CompileError> {
    let cli = Cli::parse();
    match cli.command {
        Command::Run { input } => {
            let source = read(&input)?;
            let result = rfl::run_program(&source)?;
            print!("{}", result.stdout);
            Ok(result.status)
        }
        Command::Check { input } => {
            let source = read(&input)?;
            rfl::compile_to_ir(&source)?;
            Ok(0)
        }
        Command::Compile { input, output, emit_ir } => {
            let source = read(&input)?;
            if emit_ir {
                print!("{}", rfl::compile_to_ir_text(&source)?);
                return Ok(0);
            }
            let asm = rfl::compile_to_asm(&source)?;
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

fn read(path: &std::path::Path) -> Result<String, rfl::CompileError> {
    std::fs::read_to_string(path).map_err(|err| {
        rfl::CompileError::plain(format!("cannot read {}: {err}", path.display()))
    })
}
