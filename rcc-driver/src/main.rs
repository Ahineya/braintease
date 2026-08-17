//! Ripple C99 Compiler Driver
//! 
//! Main entry point for the Ripple C99 compiler. For M1, this provides
//! a simple command-line interface for testing the backend code generation.

use clap::{Parser, Subcommand};
// use rcc_codegen::generate_assembly; // Not used currently
use rcc_backend::{lower_module_to_assembly_with_options, LoweringOptions};
use rcc_frontend::{Frontend, SemanticAnalyzer};
use rcc_preprocessor::Preprocessor;
use std::fs;
use std::path::PathBuf;
use serde::Serialize;
use rcc_common::CompilerError;

/// Configuration for compilation
#[derive(Debug, Clone)]
struct CompileConfig {
    /// Input file path
    input_path: PathBuf,
    /// Optional output file path
    output_path: Option<PathBuf>,
    /// Emit IR to stdout and exit
    emit_ir: bool,
    /// Print IR before lowering
    print_ir: bool,
    /// Save IR to file
    save_ir: bool,
    /// Custom IR output path
    ir_output_path: Option<PathBuf>,
    /// Bank size for target architecture
    bank_size: u16,
    /// Enable spill/reload tracing
    trace_spills: bool,
    /// Skip preprocessing
    no_preprocess: bool,
    /// Include directories for preprocessor
    include_dirs: Vec<PathBuf>,
    /// Preprocessor defines
    defines: Vec<String>,
    /// Enable tracing of all compilation stages to JSON files
    trace: bool,
}

#[derive(Parser)]
#[command(name = "rcc")]
#[command(about = "Ripple C99 Compiler")]
#[command(version = "0.1.0")]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Test the backend code generation with built-in examples
    Test {
        /// Which test to run
        #[arg(short, long, default_value = "hello")]
        test_name: String,
        
        /// Output file for generated assembly
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
    
    /// Generate assembly from simple IR (for M1 testing)
    GenerateAsm {
        /// Input IR description (JSON format planned)
        #[arg(short, long)]
        input: Option<PathBuf>,
        
        /// Output assembly file
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
    
    /// Compile C99 source file
    Compile {
        /// Input C99 source file
        input: PathBuf,
        
        /// Output assembly file
        #[arg(short, long)]
        output: Option<PathBuf>,
        
        /// Emit IR to stdout and exit (do not lower to assembly)
        #[arg(long)]
        emit_ir: bool,
        
        /// Print IR to stdout before lowering
        #[arg(long)]
        print_ir: bool,
        
        /// Save IR to file with .ir extension
        #[arg(long)]
        save_ir: bool,
        
        /// Specify output path for IR file (used with --save-ir)
        #[arg(long)]
        ir_output: Option<PathBuf>,
        
        /// Debug level (0=none, 1=basic, 2=verbose, 3=trace)
        #[arg(short, long, default_value = "0")]
        debug: u8,
        
        /// Bank size in cells (default: 64000, matching runtime and rct)
        #[arg(long, default_value = "64000")]
        bank_size: u16,
        
        /// Enable spill/reload tracing for debugging register allocation
        #[arg(long)]
        trace_spills: bool,
        
        /// Skip preprocessing (treat input as already preprocessed)
        #[arg(long)]
        no_preprocess: bool,
        
        /// Include directories for preprocessor
        #[arg(short = 'I', long = "include", value_name = "DIR")]
        include_dirs: Vec<PathBuf>,
        
        /// Define macro for preprocessor
        #[arg(short = 'D', long = "define", value_name = "NAME[=VALUE]")]
        defines: Vec<String>,
        
        /// Enable tracing of all compilation stages to JSON files
        #[arg(long)]
        trace: bool,
    },
}

fn main() {
    let cli = Cli::parse();
    
    match cli.command {
        Commands::Test { test_name, output } => {
            if let Err(e) = run_test(&test_name, output.as_deref()) {
                eprintln!("Error running test: {e}");
                std::process::exit(1);
            }
        }
        Commands::GenerateAsm { input, output } => {
            if let Err(e) = generate_asm_command(input.as_deref(), output.as_deref()) {
                eprintln!("Error generating assembly: {e}");
                std::process::exit(1);
            }
        }
        Commands::Compile { input, output, emit_ir, print_ir, save_ir, ir_output, debug, bank_size, trace_spills, no_preprocess, include_dirs, defines, trace } => {
            // Initialize logger based on debug level
            let log_level = match debug {
                0 => "error",
                1 => "warn",
                2 => "info",
                3 => "debug",
                _ => "trace",
            };
            
            env_logger::Builder::from_env(env_logger::Env::default().default_filter_or(log_level))
                .format_timestamp(None)
                .format_module_path(true)
                .format_target(false)
                .init();
            
            let config = CompileConfig {
                input_path: input,
                output_path: output,
                emit_ir,
                print_ir,
                save_ir,
                ir_output_path: ir_output,
                bank_size,
                trace_spills,
                no_preprocess,
                include_dirs,
                defines,
                trace,
            };
            
            if let Err(e) = compile_c99_file(config) {
                eprintln!("Error compiling C99 file: {e}");
                std::process::exit(1);
            }
        }
    }
}

fn run_test(_test_name: &str, _output_path: Option<&std::path::Path>) -> Result<(), Box<dyn std::error::Error>> {
    // Test command temporarily disabled while migrating to v2
    println!("Test command is temporarily disabled during v2 migration");
    println!("Please use the 'compile' command to test compilation");
    Ok(())
}

fn generate_asm_command(
    _input_path: Option<&std::path::Path>,
    _output_path: Option<&std::path::Path>,
) -> Result<(), Box<dyn std::error::Error>> {
    // TODO: Implement IR file parsing and assembly generation
    // For now, just show that the command exists
    println!("Generate assembly command - not yet implemented");
    println!("This will be implemented in a future milestone when we have a real IR format");
    Ok(())
}

/// Tracing helper struct to save semantic analysis state
#[derive(Serialize)]
struct SemanticAnalysisOutput {
    /// Symbol table after semantic analysis
    symbols: Vec<SymbolInfo>,
    /// Type definitions discovered
    type_definitions: Vec<TypeDefInfo>,
}

#[derive(Serialize)]
struct SymbolInfo {
    name: String,
    symbol_type: String,
    scope_level: usize,
}

#[derive(Serialize)]
struct TypeDefInfo {
    name: String,
    definition: String,
}

fn save_trace_file<T: Serialize>(
    data: &T,
    base_path: &PathBuf,
    suffix: &str,
) -> Result<(), CompilerError> {
    let mut path = base_path.clone();
    let stem = path.file_stem()
        .ok_or_else(|| CompilerError::internal_error("Invalid file path".to_string()))?
        .to_str()
        .ok_or_else(|| CompilerError::internal_error("Invalid file path encoding".to_string()))?;
    path.set_file_name(format!("{}.{}", stem, suffix));
    
    let json = serde_json::to_string_pretty(data)
        .map_err(|e| CompilerError::internal_error(format!("Failed to serialize to JSON: {}", e)))?;
    
    fs::write(&path, json)
        .map_err(|e| CompilerError::internal_error(format!("Failed to write trace file: {}", e)))?;
    
    println!("Trace saved to: {}", path.display());
    Ok(())
}

fn compile_c99_file(config: CompileConfig) -> Result<(), Box<dyn std::error::Error>> {
    // Don't print compilation message if we're just emitting IR
    if !config.emit_ir {
        println!("Compiling C99 file: {} (bank_size: {})", config.input_path.display(), config.bank_size);
    }
    
    // Read source file
    let mut source = fs::read_to_string(&config.input_path)?;
    
    // Preprocess unless explicitly disabled
    if !config.no_preprocess {
        let mut preprocessor = Preprocessor::new();
        
        // Add include directories
        for dir in config.include_dirs {
            preprocessor.add_include_dir(dir);
        }
        
        // Process defines
        for define in config.defines {
            if let Some((name, value)) = define.split_once('=') {
                preprocessor.define(name.to_string(), Some(value.to_string()));
            } else {
                preprocessor.define(define, None);
            }
        }
        
        // Preprocess the source
        source = preprocessor.process(&source, config.input_path.clone())?;
        
        if !config.emit_ir {
            println!("Successfully preprocessed source");
        }
    }
    
    // Tokenize source if tracing is enabled
    if config.trace {
        let tokens = Frontend::tokenize_source(&source)?;
        save_trace_file(&tokens, &config.input_path, "tokens.json")?;
        if !config.emit_ir {
            println!("Saved {} tokens to trace file", tokens.len());
        }
    }
    
    // Parse the source
    let mut ast = match Frontend::parse_source(&source) {
        Ok(ast) => {
            if !config.emit_ir {
                println!("Successfully parsed C99 source...");
                println!("Found {} top-level items", ast.items.len());
                
                for item in &ast.items {
                    match item {
                        rcc_frontend::TopLevelItem::Function(func) => {
                            println!("  Function: {} -> {}", func.name, func.return_type);
                        }
                        rcc_frontend::TopLevelItem::Declarations(decls) => {
                            for decl in decls {
                                if decl.storage_class == rcc_frontend::StorageClass::Typedef {
                                    println!("  Typedef: {} = {}", decl.name, decl.decl_type);
                                } else {
                                    println!("  Global variable: {} : {}", decl.name, decl.decl_type);
                                }
                            }
                        }
                        rcc_frontend::TopLevelItem::TypeDefinition { name, .. } => {
                            println!("  Type definition: {name}");
                        }
                    }
                }
            }
            ast
        }
        Err(e) => {
            eprintln!("Parse error: {e}");
            return Err(e.into());
        }
    };
    
    // Save AST if tracing is enabled
    if config.trace {
        save_trace_file(&ast, &config.input_path, "ast.json")?;
        if !config.emit_ir {
            println!("Saved AST to trace file");
        }
    }
    
    // Perform semantic analysis
    let mut analyzer = SemanticAnalyzer::new();
    analyzer.analyze(&mut ast)?;
    let type_analyzer = analyzer.into_type_info();
    
    // Save semantic analysis output if tracing is enabled
    if config.trace {
        // Extract symbol table and type definitions from the TypeAnalyzer
        let mut symbols = Vec::new();
        let mut type_defs = Vec::new();
        
        // Get all symbols from the symbol table
        let symbol_table = type_analyzer.borrow().symbol_table.clone();
        let symbol_types = type_analyzer.borrow().symbol_types.clone();
        
        // Iterate through all symbols and their types
        for (symbol_id, sym_type) in symbol_types.borrow().iter() {
            // Try to find the symbol name by looking it up
            if let Some(symbol) = symbol_table.borrow().get_symbol(*symbol_id) {
                symbols.push(SymbolInfo {
                    name: symbol.name.clone(),
                    symbol_type: format!("{}", sym_type),
                    scope_level: 0, // TODO: Could track actual scope level
                });
            }
        }
        
        // Get all type definitions
        let type_definitions = type_analyzer.borrow().type_definitions.clone();
        for (name, typedef) in type_definitions.borrow().iter() {
            type_defs.push(TypeDefInfo {
                name: name.clone(),
                definition: format!("{}", typedef),
            });
        }
        
        let semantic_output = SemanticAnalysisOutput {
            symbols,
            type_definitions: type_defs,
        };
        save_trace_file(&semantic_output, &config.input_path, "sem.json")?;
        if !config.emit_ir {
            println!("Saved semantic analysis output to trace file");
        }
    }
    
    // Convert to typed AST
    let typed_ast = rcc_frontend::type_translation_unit(&ast, type_analyzer)
        .map_err(|e| CompilerError::internal_error(format!("Type error: {}", e)))?;
    
    // Save typed AST if tracing is enabled
    if config.trace {
        save_trace_file(&typed_ast, &config.input_path, "tast.json")?;
        if !config.emit_ir {
            println!("Saved typed AST to trace file");
        }
    }
    
    // Generate IR from typed AST
    if !config.emit_ir {
        println!("\n💀 Attempting full compilation pipeline");
    }
    let codegen = rcc_frontend::TypedCodeGenerator::new(config.input_path.file_stem().unwrap().to_str().unwrap().to_string());
    match codegen.generate(&typed_ast) {
        Ok(ir_module) => {
            if !config.emit_ir {
                println!("💫 Successfully generated IR");
                println!("🦄 Module contains {} functions", ir_module.functions.len());
            }
            
            // Save IR as JSON if tracing is enabled
            if config.trace {
                save_trace_file(&ir_module, &config.input_path, "ir.json")?;
                if !config.emit_ir {
                    println!("Saved IR to trace file");
                }
            }
            
            // Format IR output
            let mut ir_output = String::new();
            
            // Add global declarations at the top for better debugging
            if !ir_module.globals.is_empty() {
                ir_output.push_str("; Global variables:\n");
                for global in &ir_module.globals {
                    ir_output.push_str(&format!("; @{} : {:?}", global.name, global.var_type));
                    if let Some(init) = &global.initializer {
                        // Show initializer in a readable format
                        match init {
                            rcc_frontend::ir::Value::Constant(val) => {
                                ir_output.push_str(&format!(" = {val}"));
                            }
                            rcc_frontend::ir::Value::ConstantArray(values) => {
                                // If it looks like a string, show it as a string
                                if values.last() == Some(&0) && 
                                   values[..values.len().saturating_sub(1)].iter()
                                       .all(|&v| (32..=126).contains(&v)) {
                                    let s: String = values[..values.len().saturating_sub(1)].iter()
                                        .map(|&c| c as u8 as char).collect();
                                    ir_output.push_str(&format!(" = \"{s}\""));
                                } else {
                                    ir_output.push_str(&format!(" = {values:?}"));
                                }
                            }
                            _ => {
                                ir_output.push_str(&format!(" = {init:?}"));
                            }
                        }
                    }
                    ir_output.push('\n');
                }
                ir_output.push('\n');
            }
            
            // String literals are included in globals now, so we don't need a separate section
            
            for func in &ir_module.functions {
                ir_output.push_str(&format!("define {} {{\n", func.name));
                for (param_id, param_type) in &func.parameters {
                    ir_output.push_str(&format!("  param %{param_id}: {param_type:?}\n"));
                }
                for block in &func.blocks {
                    ir_output.push_str(&format!("L{}:\n", block.id));
                    for inst in &block.instructions {
                        ir_output.push_str(&format!("  {inst}\n"));
                    }
                }
                ir_output.push_str("}\n");
            }
            
            // If --emit-ir is set, just print IR and exit
            if config.emit_ir {
                print!("{ir_output}");
                return Ok(());
            }
            
            // Print IR if requested (when not using --emit-ir)
            if config.print_ir {
                println!("\n=== IR Output ===");
                print!("{ir_output}");
                println!("=== End IR ===\n");
            }
            
            // Save IR to file if requested
            if config.save_ir {
                let ir_path = if let Some(path) = config.ir_output_path {
                    // Use the specified IR output path
                    path
                } else {
                    // Default: save next to input file with .ir extension
                    let mut path = config.input_path.clone();
                    path.set_extension("ir");
                    path
                };
                fs::write(&ir_path, &ir_output)?;
                println!("IR saved to: {}", ir_path.display());
            }
            
            // Check if main function exists
            let has_main = ir_module.functions.iter().any(|f| f.name == "main");
            
            // Lower Module to assembly with bank_size parameter
            let options = LoweringOptions {
                bank_size: config.bank_size,
                use_v2: true,
                trace_spills: config.trace_spills,
            };
            match lower_module_to_assembly_with_options(&ir_module, options) {
                Ok(asm_instructions) => {
                    println!("💕 Successfully lowered to assembly");
                    
                    // Generate assembly text
                    let asm_text = rcc_codegen::emit::emit_complete_program(asm_instructions, has_main)?;
                    
                    // Write to output file
                    let final_output_path = if let Some(path) = config.output_path {
                        path
                    } else {
                        let mut path = config.input_path.clone();
                        path.set_extension("asm");
                        path
                    };
                    
                    fs::write(&final_output_path, asm_text)?;
                    println!("Assembly written to: {}", final_output_path.display());
                    Ok(())
                }
                Err(e) => {
                    eprintln!("Error: Failed to lower IR to assembly: {e}");
                    eprintln!("Note: Code generation is still under development for complex features");
                    Err(e.into())
                }
            }
        }
        Err(e) => {
            eprintln!("Error: Failed to generate IR: {e}");
            eprintln!("Note: Code generation is still under development for complex features");
            Err(e.into())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    
    #[test]
    fn test_hello_world_generation() {
        // Test that we can generate hello world without panicking
        let result = run_test("hello", None);
        assert!(result.is_ok());
    }
    
    #[test]
    fn test_arithmetic_generation() {
        // Test that we can generate arithmetic test without panicking
        let result = run_test("arithmetic", None);
        assert!(result.is_ok());
    }
}