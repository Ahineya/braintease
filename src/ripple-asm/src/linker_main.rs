use clap::Parser;
use ripple_asm::{Linker, MacroFormatter, Archive, ObjectFile};
use std::fs;
use std::io::{BufWriter, Write};
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
    
    /// Output format (binary, text, macro, bf, archive, sys, rxe)
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
    
    /// Generate standalone CPU output (macro or bf format)
    #[arg(short = 's', long = "standalone")]
    standalone: bool,
    
    /// Enable debug mode in standalone macro output (sets DEBUG to 1)
    #[arg(short = 'd', long = "debug")]
    debug: bool,

    /// Export the instrumented Brainfuck host protocol (requires macro or bf --standalone)
    #[arg(long)]
    bf_host: bool,

    /// Enable Brainfuck CPU interrupts, CSRs and instruction-count timer (off by default)
    #[arg(long)]
    bf_irq: bool,

    /// RAM banks declared by --bf-host; this is a limit, not inferred usage
    #[arg(long, default_value = "16")]
    memory_banks: u16,

    /// Reserve additional code banks for STORC-loaded programs (host BF only)
    #[arg(long, default_value = "0")]
    code_banks: u16,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();
    if cli.code_banks != 0 && (!cli.bf_host || !cli.standalone || !matches!(cli.format.as_str(), "macro" | "bf")) {
        return Err("--code-banks requires --bf-host --standalone with macro or bf output".into());
    }
    if cli.bf_irq && (!cli.standalone || !matches!(cli.format.as_str(), "macro" | "bf")) {
        return Err("--bf-irq requires standalone Brainfuck or macro output".into());
    }
    if cli.bf_host && (!matches!(cli.format.as_str(), "macro" | "bf") || !cli.standalone) {
        return Err("--bf-host requires --format macro or bf with --standalone".into());
    }
    if cli.format == "bf" && (!cli.bf_host || !cli.standalone) {
        return Err("--format bf requires --standalone --bf-host".into());
    }
    
    if cli.inputs.is_empty() {
        eprintln!("Error: No input files specified");
        std::process::exit(1);
    }
    
    // Load all object files (with names for archive)
    let mut object_files = Vec::new();
    let mut archives = Vec::new();
    let mut named_object_files = Vec::new();
    for input_path in &cli.inputs {
        let content = fs::read_to_string(input_path)?;
        
        // Try to load as archive first
        if let Ok(archive) = serde_json::from_str::<Archive>(&content) {
            println!("Loaded archive {}", input_path.display());
            if cli.format == "archive" {
                named_object_files.extend(archive.objects.into_iter().map(|entry| (entry.name, entry.object)));
            } else {
                archives.push(archive);
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
                    if cli.format == "archive" {
                        named_object_files.push((name, obj));
                    } else {
                        object_files.push(obj);
                    }
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
    match linker.link_archives(object_files, archives) {
        Ok(mut program) => {
            if cli.format == "sys" {
                program.apply_code_bank_base(cli.code_bank_base);
            }
            let output_path = cli.output.unwrap_or_else(|| {
                PathBuf::from(match cli.format.as_str() {
                    "text" => "a.txt",
                    "macro" => "a.bfm",
                    "bf" => "a.bf",
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
                    if program.instructions.len() >= usize::from(program.bank_size.max(1)) * 65536 {
                        return Err("BF code image and final HALT must fit the 16-bit PC/PCB address space".into());
                    }
                    let formatter = MacroFormatter::new().with_irq(cli.bf_irq).with_code_banks(cli.code_banks);
                    let macro_output = if cli.bf_host {
                        formatter.format_linked_program_host(&program, cli.memory_banks)?
                    } else if cli.standalone {
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
                "bf" => {
                    // A temporary sibling prevents failed exports from replacing
                    // a previously complete image. No expanded source is retained
                    // in memory, including for multi-million-instruction images.
                    let temporary = output_path.with_extension(format!("bf.{}.tmp", std::process::id()));
                    let result = (|| -> Result<(), Box<dyn std::error::Error>> {
                        let file = fs::File::create(&temporary)?;
                        let mut writer = BufWriter::new(file);
                        MacroFormatter::new().with_irq(cli.bf_irq).with_code_banks(cli.code_banks).write_linked_program_host_bf(&program, cli.memory_banks, &mut writer)?;
                        writer.flush()?;
                        drop(writer);
                        fs::rename(&temporary, &output_path)?;
                        Ok(())
                    })();
                    if result.is_err() { let _ = fs::remove_file(&temporary); }
                    result?;
                    println!("✓ Linked to compact Brainfuck {}", output_path.display());
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
