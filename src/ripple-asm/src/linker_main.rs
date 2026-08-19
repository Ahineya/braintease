use clap::Parser;
use ripple_asm::{Linker, MacroFormatter, Archive, ObjectFile};
use std::fs;
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "rlink")]
#[command(about = "Ripple Linker - Links object files into executable")]
#[command(version)]
struct Cli {
    /// Input object files (.pobj)
    inputs: Vec<PathBuf>,
    
    /// Output file
    #[arg(short, long)]
    output: Option<PathBuf>,
    
    /// Output format (binary, text, macro, archive, sys, rxe)
    #[arg(short, long, default_value = "binary")]
    format: String,
    
    /// Bank size
    #[arg(short, long, default_value = "16")]
    bank_size: u16,

    /// Add this to every JAL code bank (SYS images loaded away from PCB 0)
    #[arg(long, default_value = "0")]
    code_bank_base: u16,

    /// GP bank written into SYS1 header
    #[arg(long, default_value = "1")]
    gp_bank: u16,

    /// SB bank written into SYS1 header
    #[arg(long, default_value = "2")]
    sb_bank: u16,
    
    /// Generate standalone macro file with CPU template (for macro format only)
    #[arg(short = 's', long = "standalone")]
    standalone: bool,
    
    /// Enable debug mode in standalone macro output (sets DEBUG to 1)
    #[arg(short = 'd', long = "debug")]
    debug: bool,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();
    
    if cli.inputs.is_empty() {
        eprintln!("Error: No input files specified");
        std::process::exit(1);
    }
    
    // Load all object files (with names for archive)
    let mut object_files = Vec::new();
    let mut named_object_files = Vec::new();
    for input_path in &cli.inputs {
        let content = fs::read_to_string(input_path)?;
        
        // Try to load as archive first
        if let Ok(archive) = serde_json::from_str::<Archive>(&content) {
            println!("Loaded archive {}", input_path.display());
            let extracted = Linker::extract_from_archive(&archive);
            object_files.extend(extracted);
            // For archive creation, preserve the archive name
            for entry in archive.objects {
                named_object_files.push((entry.name, entry.object));
            }
        } else {
            // Try as regular object file
            match serde_json::from_str::<ObjectFile>(&content) {
                Ok(obj) => {
                    println!("Loaded {}", input_path.display());
                    let name = input_path.file_stem()
                        .unwrap_or_default()
                        .to_string_lossy()
                        .into_owned();
                    named_object_files.push((name, obj.clone()));
                    object_files.push(obj);
                }
                Err(e) => {
                    eprintln!("Failed to parse {}: {}", input_path.display(), e);
                    std::process::exit(1);
                }
            }
        }
    }
    
    // Handle archive creation separately
    if cli.format == "archive" {
        let archive = Linker::create_archive(named_object_files);
        let output_path = cli.output.unwrap_or_else(|| PathBuf::from("lib.par"));
        let json = serde_json::to_string_pretty(&archive)?;
        fs::write(&output_path, json)?;
        println!("✓ Created archive {}", output_path.display());
        println!("  Contains {} object files", archive.objects.len());
        for entry in &archive.objects {
            println!("    - {}", entry.name);
        }
        return Ok(());
    }
    
    // Link the object files
    let linker = Linker::new(cli.bank_size);
    match linker.link(object_files) {
        Ok(mut program) => {
            if cli.format == "sys" {
                program.apply_code_bank_base(cli.code_bank_base);
            }
            let output_path = cli.output.unwrap_or_else(|| {
                PathBuf::from(match cli.format.as_str() {
                    "text" => "a.txt",
                    "macro" => "a.bfm",
                    "sys" => "a.sys",
                    "rxe" => "a.rxe",
                    _ => "a.out",
                })
            });
            
            match cli.format.as_str() {
                "binary" => {
                    let binary = program.to_binary();
                    fs::write(&output_path, binary)?;
                    println!("✓ Linked to binary {}", output_path.display());
                }
                "sys" => {
                    let binary = program.to_sys(cli.code_bank_base, cli.gp_bank, cli.sb_bank);
                    fs::write(&output_path, binary)?;
                    println!("✓ Linked to SYS1 {}", output_path.display());
                }
                "rxe" => {
                    let binary = program.to_rxe();
                    fs::write(&output_path, binary)?;
                    println!("✓ Linked to RXE1 {}", output_path.display());
                }
                "text" => {
                    let text = program.to_text();
                    fs::write(&output_path, text)?;
                    println!("✓ Linked to text {}", output_path.display());
                }
                "macro" => {
                    if program.code_bank_count() > 1 {
                        eprintln!("Error: BF CPU cannot seek code banks yet");
                        eprintln!(
                            "  Linked program needs {} code banks (bank size {})",
                            program.code_bank_count(),
                            program.bank_size
                        );
                        std::process::exit(1);
                    }
                    let formatter = MacroFormatter::new();
                    let macro_output = if cli.standalone {
                        formatter.format_linked_program_standalone(&program, cli.debug)
                    } else {
                        formatter.format_linked_program(&program)
                    };
                    fs::write(&output_path, macro_output)?;
                    println!("✓ Linked to macro file {}", output_path.display());
                    if cli.standalone {
                        let debug_msg = if cli.debug { " with DEBUG=1" } else { "" };
                        println!("  (Standalone mode - includes CPU template{})", debug_msg);
                    }
                }
                _ => {
                    eprintln!("Unknown format: {}", cli.format);
                    std::process::exit(1);
                }
            }
            
            // Print statistics
            println!("  Entry point: 0x{:08X}", program.entry_point);
            println!("  Instructions: {}", program.instructions.len());
            println!("  Data: {} bytes", program.data.len());
            println!("  Labels: {}", program.labels.len());
        }
        Err(errors) => {
            eprintln!("Linking failed:");
            for error in errors {
                eprintln!("  - {}", error);
            }
            std::process::exit(1);
        }
    }
    
    Ok(())
}