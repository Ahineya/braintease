use clap::{Parser, Subcommand};
use ripple_asm::{RippleAssembler, AssemblerOptions, MacroFormatter, Opcode, Register, InstructionFormat, Disassembler};
use ripple_asm::virtual_instructions::VirtualInstructionRegistry;
use std::fs;
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "rasm")]
#[command(about = "Ripple Assembler - Assembles Ripple assembly files")]
#[command(version)]
struct Cli {
    /// Show instruction reference
    #[arg(short, long)]
    reference: bool,
    
    #[command(subcommand)]
    command: Option<Commands>,
}

#[derive(Subcommand)]
enum Commands {
    /// Assemble source file to object file
    Assemble {
        /// Input assembly file (.asm)
        input: PathBuf,
        
        /// Output object file (.pobj)
        #[arg(short, long)]
        output: Option<PathBuf>,
        
        /// Output format (json, binary, macro)
        #[arg(short, long, default_value = "json")]
        format: String,
        
        /// Bank size
        #[arg(short, long, default_value = "16")]
        bank_size: u16,
        
        /// Maximum immediate value
        #[arg(short, long, default_value = "65535")]
        max_immediate: u32,
        
        /// Memory offset for data addresses (default 2 for VM special values)
        #[arg(short = 'm', long, default_value = "2")]
        memory_offset: u16,
        
        /// Case insensitive parsing
        #[arg(long, default_value = "true")]
        case_insensitive: bool,
    },
    
    /// Convert object file to macro format
    Format {
        /// Input object file (.pobj)
        input: PathBuf,
        
        /// Output macro file (.bfm)
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
    
    /// Validate assembly file
    Check {
        /// Input assembly file
        input: PathBuf,
    },
    
    /// Disassemble binary file to assembly
    Disassemble {
        /// Input binary file (.bin)
        input: PathBuf,
        
        /// Output assembly file (.asm)
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
}

fn get_instruction_format(opcode: Opcode) -> &'static str {
    match opcode.format() {
        InstructionFormat::R => match opcode {
            Opcode::Nop => "",
            Opcode::Brk => "",
            Opcode::Loadc | Opcode::Storc => "R0, bank, addr",
            _ => "rd, rs1, rs2",
        },
        InstructionFormat::I => match opcode {
            Opcode::Addi | Opcode::Andi | Opcode::Ori | Opcode::Xori | 
            Opcode::Slli | Opcode::Srli | Opcode::Muli | Opcode::Divi | Opcode::Modi => "rd, rs, imm",
            Opcode::Load => "rd, bank, addr",
            Opcode::Store => "rs, bank, addr", 
            Opcode::Beq | Opcode::Bne | Opcode::Blt | Opcode::Bge => "rs1, rs2, label",
            Opcode::Jal => "rd, target",
            _ => "rd, rs, imm",
        },
        InstructionFormat::I1 => "rd, imm",
    }
}

fn get_instruction_description(opcode: Opcode) -> &'static str {
    match opcode {
        // Arithmetic
        Opcode::Add => "rd = rs1 + rs2",
        Opcode::Sub => "rd = rs1 - rs2",
        Opcode::Mul => "rd = rs1 * rs2",
        Opcode::Div => "rd = rs1 / rs2",
        Opcode::Mod => "rd = rs1 % rs2",
        Opcode::Addi => "rd = rs + imm",
        Opcode::Muli => "rd = rs * imm",
        Opcode::Divi => "rd = rs / imm",
        Opcode::Modi => "rd = rs % imm",
        
        // Logical
        Opcode::And => "rd = rs1 & rs2",
        Opcode::Or => "rd = rs1 | rs2",
        Opcode::Xor => "rd = rs1 ^ rs2",
        Opcode::Andi => "rd = rs & imm",
        Opcode::Ori => "rd = rs | imm",
        Opcode::Xori => "rd = rs ^ imm",
        
        // Shift
        Opcode::Sll => "rd = rs1 << rs2",
        Opcode::Srl => "rd = rs1 >> rs2",
        Opcode::Slli => "rd = rs << imm",
        Opcode::Srli => "rd = rs >> imm",
        
        // Comparison
        Opcode::Slt => "rd = (rs1 < rs2) ? 1 : 0 (signed)",
        Opcode::Sltu => "rd = (rs1 < rs2) ? 1 : 0 (unsigned)",
        
        // Memory
        Opcode::Li => "rd = imm",
        Opcode::Load => "rd = memory[bank][addr]",
        Opcode::Store => "memory[bank][addr] = rs",
        Opcode::Loadc => "X0..X3 = IMEM[bank][addr] (4 instruction cells)",
        Opcode::Storc => "IMEM[bank][addr] = X0..X3 (4 instruction cells)",
        
        // Control flow
        Opcode::Jal => "rd = PC + 1; PC = target",
        Opcode::Jalr => "rd = PC + 1; PC = rs + offset",
        Opcode::Beq => "if (rs1 == rs2) PC += offset",
        Opcode::Bne => "if (rs1 != rs2) PC += offset",
        Opcode::Blt => "if (rs1 < rs2) PC += offset",
        Opcode::Bge => "if (rs1 >= rs2) PC += offset",
        
        // Special
        Opcode::Nop => "No operation",
        Opcode::Brk => "Break/Debug",
    }
}

fn get_instruction_category(opcode: Opcode) -> &'static str {
    match opcode {
        Opcode::Add | Opcode::Sub | Opcode::Mul | Opcode::Div | Opcode::Mod |
        Opcode::Addi | Opcode::Muli | Opcode::Divi | Opcode::Modi => "ARITHMETIC",
        
        Opcode::And | Opcode::Or | Opcode::Xor |
        Opcode::Andi | Opcode::Ori | Opcode::Xori => "LOGICAL",
        
        Opcode::Sll | Opcode::Srl | Opcode::Slli | Opcode::Srli => "SHIFT",
        
        Opcode::Slt | Opcode::Sltu => "COMPARISON",
        
        Opcode::Li | Opcode::Load | Opcode::Store | Opcode::Loadc | Opcode::Storc => "MEMORY",
        
        Opcode::Jal | Opcode::Jalr | Opcode::Beq | Opcode::Bne | 
        Opcode::Blt | Opcode::Bge => "CONTROL FLOW",
        
        Opcode::Nop | Opcode::Brk => "SPECIAL",
    }
}

fn print_instruction_reference() {
    println!("RIPPLE ASSEMBLER INSTRUCTION REFERENCE");
    println!("=====================================\n");
    
    // Group instructions by category
    let mut categories: std::collections::HashMap<&str, Vec<Opcode>> = std::collections::HashMap::new();
    
    // Collect all opcodes and group them
    for mnemonic in Opcode::all() {
        if let Some(opcode) = Opcode::from_str(mnemonic) {
            let category = get_instruction_category(opcode);
            categories.entry(category).or_insert_with(Vec::new).push(opcode);
        }
    }
    
    // Print each category
    let category_order = ["ARITHMETIC", "LOGICAL", "SHIFT", "COMPARISON", "MEMORY", "CONTROL FLOW", "SPECIAL"];
    
    for category in category_order {
        if let Some(opcodes) = categories.get(category) {
            println!("{} INSTRUCTIONS", category);
            println!("{}", "-".repeat(category.len() + 13));
            
            for opcode in opcodes {
                let mnemonic = opcode.to_str();
                let format = get_instruction_format(*opcode);
                let description = get_instruction_description(*opcode);
                
                if format.is_empty() {
                    println!("{:<8}             # {}", mnemonic, description);
                } else {
                    println!("{:<8} {:<20} # {}", mnemonic, format, description);
                }
            }
            println!();
        }
    }
    
    // Add HALT as special case
    println!("SPECIAL INSTRUCTION (ALIAS)");
    println!("---------------------------");
    println!("HALT                 # Stop execution (NOP with all zeros)");
    println!();
    
    // Virtual instructions
    println!("VIRTUAL INSTRUCTIONS");
    println!("--------------------");
    let registry = VirtualInstructionRegistry::new();
    for name in registry.get_all_names() {
        let (format, description) = match name.as_str() {
            "MOVE" => ("rd, rs", "rd = rs (expands to: ADD rd, rs, R0)"),
            "INC" => ("rd", "rd = rd + 1 (expands to: ADDI rd, rd, 1)"),
            "DEC" => ("rd", "rd = rd - 1 (expands to: ADDI rd, rd, -1)"),
            "PUSH" => ("rs", "Push to stack (expands to 2 instructions)"),
            "POP" => ("rd", "Pop from stack (expands to 2 instructions)"),
            "CALL" => ("target", "Call subroutine (expands to: JAL RA, target)"),
            "RET" => ("", "Return from subroutine (expands to: JALR R0, RA, 0)"),
            _ => ("", "Custom virtual instruction"),
        };
        
        if format.is_empty() {
            println!("{:<8}             # {}", name, description);
        } else {
            println!("{:<8} {:<20} # {}", name, format, description);
        }
    }
    println!();
    
    // Registers
    println!("REGISTERS");
    println!("---------");
    for reg in Register::all() {
        let description = match reg {
            "R0" | "R3" | "R4" | "R5" | "R6" | "R7" | "R8" | "R9" | 
            "R10" | "R11" | "R12" | "R13" | "R14" | "R15" => "General purpose register",
            "PC" => "Program counter",
            "PCB" => "Program counter bank",
            "RA" => "Return address",
            "RAB" => "Return address bank",
            _ => "Unknown register",
        };
        println!("{:<8}             # {}", reg, description);
    }
    println!();
    
    println!("ASSEMBLER DIRECTIVES");
    println!("--------------------");
    println!(".data                # Start data section");
    println!(".code/.text          # Start code section");
    println!(".byte/.db value,...  # Define bytes");
    println!(".word/.dw value,...  # Define words (16-bit)");
    println!(".asciiz \"string\"     # Define null-terminated string");
    println!(".string \"string\"     # Define string (no null terminator)");
    println!();
    
    println!("SYNTAX");
    println!("------");
    println!("label:               # Define a label");
    println!("# comment            # Hash comment");
    println!("; comment            # Semicolon comment");
    println!("// comment           # C-style comment");
    println!("0x1234               # Hexadecimal number");
    println!("0b1010               # Binary number");
    println!("42                   # Decimal number");
    println!("-42                  # Negative number");
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();
    
    // Handle reference flag
    if cli.reference {
        print_instruction_reference();
        return Ok(());
    }
    
    // If no command provided and not reference, show help
    let command = cli.command.ok_or("No command provided. Use --help for usage information.")?;
    
    match command {
        Commands::Assemble { 
            input, 
            output, 
            format, 
            bank_size, 
            max_immediate,
            memory_offset,
            case_insensitive,
        } => {
            let source = fs::read_to_string(&input)?;
            
            let options = AssemblerOptions {
                case_insensitive,
                start_bank: 0,
                bank_size,
                max_immediate,
                memory_offset,
            };
            
            let assembler = RippleAssembler::new(options);
            
            match assembler.assemble(&source) {
                Ok(obj) => {
                    let output_path = output.unwrap_or_else(|| {
                        let mut path = input.clone();
                        path.set_extension(match format.as_str() {
                            "binary" => "bin",
                            "macro" => "bfm",
                            _ => "pobj",
                        });
                        path
                    });
                    
                    match format.as_str() {
                        "json" => {
                            let json = serde_json::to_string_pretty(&obj)?;
                            fs::write(&output_path, json)?;
                            println!("✓ Assembled to {}", output_path.display());
                        }
                        "binary" => {
                            match assembler.assemble_to_binary(&source) {
                                Ok(binary) => {
                                    fs::write(&output_path, binary)?;
                                    println!("✓ Assembled to binary {}", output_path.display());
                                }
                                Err(errors) => {
                                    eprintln!("Assembly failed:");
                                    for error in errors {
                                        eprintln!("  - {}", error);
                                    }
                                    std::process::exit(1);
                                }
                            }
                        }
                        "macro" => {
                            let formatter = MacroFormatter::new();
                            let macro_output = formatter.format_full_program(
                                &obj.instructions,
                                Some(&obj.data),
                                None,
                                Some(&format!("Assembled from {}", input.display())),
                            );
                            fs::write(&output_path, macro_output)?;
                            println!("✓ Formatted to macro file {}", output_path.display());
                        }
                        _ => {
                            eprintln!("Unknown format: {}", format);
                            std::process::exit(1);
                        }
                    }
                    
                    // Print statistics
                    println!("  Instructions: {}", obj.instructions.len());
                    println!("  Labels: {}", obj.labels.len());
                    println!("  Data: {} bytes", obj.data.len());
                    if !obj.unresolved_references.is_empty() {
                        println!("  ⚠ Unresolved references: {}", obj.unresolved_references.len());
                        for (idx, unresolved) in &obj.unresolved_references {
                            println!("    - Instruction {}: {}", idx, unresolved.label);
                        }
                    }
                }
                Err(errors) => {
                    eprintln!("Assembly failed:");
                    for error in errors {
                        eprintln!("  - {}", error);
                    }
                    std::process::exit(1);
                }
            }
        }
        
        Commands::Format { input, output } => {
            let content = fs::read_to_string(&input)?;
            let obj: ripple_asm::ObjectFile = serde_json::from_str(&content)?;
            
            let formatter = MacroFormatter::new();
            let macro_output = formatter.format_full_program(
                &obj.instructions,
                Some(&obj.data),
                None,
                Some(&format!("Formatted from {}", input.display())),
            );
            
            let output_path = output.unwrap_or_else(|| {
                let mut path = input.clone();
                path.set_extension("bfm");
                path
            });
            
            fs::write(&output_path, macro_output)?;
            println!("✓ Formatted to {}", output_path.display());
        }
        
        Commands::Check { input } => {
            let source = fs::read_to_string(&input)?;
            let assembler = RippleAssembler::new(AssemblerOptions::default());
            
            match assembler.assemble(&source) {
                Ok(obj) => {
                    println!("✓ {} is valid", input.display());
                    println!("  Instructions: {}", obj.instructions.len());
                    println!("  Labels: {}", obj.labels.len());
                    println!("  Data: {} bytes", obj.data.len());
                    
                    if !obj.unresolved_references.is_empty() {
                        println!("  ⚠ Unresolved references:");
                        for (_, unresolved) in &obj.unresolved_references {
                            println!("    - {}", unresolved.label);
                        }
                    }
                }
                Err(errors) => {
                    eprintln!("✗ {} has errors:", input.display());
                    for error in errors {
                        eprintln!("  - {}", error);
                    }
                    std::process::exit(1);
                }
            }
        }
        
        Commands::Disassemble { input, output } => {
            let binary = fs::read(&input)?;
            let mut disassembler = Disassembler::new();
            
            match disassembler.disassemble_binary(&binary) {
                Ok(assembly) => {
                    let output_path = output.unwrap_or_else(|| {
                        let mut path = input.clone();
                        path.set_extension("asm");
                        path
                    });
                    
                    fs::write(&output_path, assembly)?;
                    println!("✓ Disassembled to {}", output_path.display());
                }
                Err(error) => {
                    eprintln!("Disassembly failed: {}", error);
                    std::process::exit(1);
                }
            }
        }
    }
    
    Ok(())
}