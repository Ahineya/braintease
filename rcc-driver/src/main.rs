//! Ripple C99 Compiler Driver
//! 
//! Main entry point for the Ripple C99 compiler. For M1, this provides
//! a simple command-line interface for testing the backend code generation.

use clap::{Parser, Subcommand};
// use rcc_codegen::generate_assembly; // Not used currently
use rcc_backend::{lower_module_to_assembly_with_options, LoweringOptions};
use rcc_frontend::{Frontend, SemanticAnalyzer};
use rcc_ir::ir::Module;
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
    segmented_stack: bool,
    /// Skip preprocessing
    no_preprocess: bool,
    /// Include directories for preprocessor
    include_dirs: Vec<PathBuf>,
    /// Preprocessor defines
    defines: Vec<String>,
    /// Enable tracing of all compilation stages to JSON files
    trace: bool,
    timings: bool,
    target: rcc_ir::target::Target,
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
    /// Link native-targeted IR translation units into a host executable
    /// Link application IR and guest libc into an independent Fuse-X executable
    LinkApp {
        #[arg(required = true)]
        inputs: Vec<PathBuf>,
        #[arg(short, long)]
        output: PathBuf,
    },
    LinkNative {
        #[arg(required = true)]
        inputs: Vec<PathBuf>,
        /// Kernel IR units whose globals are shared between hosted contexts
        #[arg(long = "kernel", value_name = "IR")]
        kernel: Vec<PathBuf>,
        #[arg(short, long)]
        output: PathBuf,
    },
    /// Test the backend code generation with built-in examples
    Test {
        /// Which test to run
        #[arg(short, long, default_value = "hello")]
        test_name: String,
        
        /// Output file for generated assembly
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
    
    /// Lower a textual IR file to assembly
    GenerateAsm {
        /// Input textual IR file
        #[arg(short, long)]
        input: PathBuf,
        
        /// Output assembly file
        #[arg(short, long)]
        output: Option<PathBuf>,

        /// Bank size in cells (default: 64000, matching runtime and rct)
        #[arg(long, default_value = "64000")]
        bank_size: u16,

        /// Enable spill/reload tracing for debugging register allocation
        #[arg(long)]
        trace_spills: bool,

        /// Use a configured pool of stack banks (requires matching runtime objects)
        #[arg(long)]
        segmented_stack: bool,
    },
    
    /// Compile C99 source file
    Compile {
        /// Input C99 source file
        input: PathBuf,
        
        /// Output assembly file
        #[arg(short, long)]
        output: Option<PathBuf>,
        
        /// Emit textual IR to stdout and exit (do not lower to assembly)
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

        /// Use a configured pool of stack banks (requires matching runtime objects)
        #[arg(long)]
        segmented_stack: bool,
        
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
        /// Report compilation stage wall times to stderr
        #[arg(long)]
        timings: bool,
        /// Native v1 uses host machine code with the shared Ripple data ABI
        #[arg(long, default_value="ripple", value_parser=rcc_ir::target::Target::parse)]
        target: rcc_ir::target::Target,
    },
}

fn main() {
    let cli = Cli::parse();
    
    match cli.command {
        Commands::LinkApp { inputs, output } => {
            let result = (|| -> Result<(),Box<dyn std::error::Error>> {
                let modules=inputs.iter().map(|p| rcc_ir::parse_module(&fs::read_to_string(p)?).map_err(Into::into)).collect::<Result<Vec<_>,Box<dyn std::error::Error>>>()?;
                rcc_native::compile_application(&modules,&output)?;
                Ok(())
            })();
            if let Err(error)=result {eprintln!("Application link failed: {error}");std::process::exit(1);}
        },
        Commands::LinkNative { inputs, kernel, output } => {
            let result = (|| -> Result<(), Box<dyn std::error::Error>> {
                let modules = inputs.iter().map(|path| {
                    let text = fs::read_to_string(path)?;
                    rcc_ir::parse_module(&text).map_err(Into::into)
                }).collect::<Result<Vec<_>, Box<dyn std::error::Error>>>()?;
                if kernel.is_empty() {
                    let module = rcc_native::link_modules(&modules)?;
                    rcc_native::compile_executable(&module, &output)?;
                } else {
                    let kernel = kernel.iter().map(|path| {
                        let text = fs::read_to_string(path)?;
                        rcc_ir::parse_module(&text).map_err(Into::into)
                    }).collect::<Result<Vec<_>, Box<dyn std::error::Error>>>()?;
                    rcc_native::compile_hosted_executable(&kernel, &modules, &output)?;
                }
                Ok(())
            })();
            if let Err(error) = result {
                eprintln!("Native link failed: {error}");
                std::process::exit(1);
            }
        },
        Commands::Test { test_name, output } => {
            if let Err(e) = run_test(&test_name, output.as_deref()) {
                eprintln!("Error running test: {e}");
                std::process::exit(1);
            }
        }
        Commands::GenerateAsm { input, output, bank_size, trace_spills, segmented_stack } => {
            if let Err(e) = generate_asm_command(&input, output.as_deref(), bank_size, trace_spills, segmented_stack) {
                eprintln!("Error generating assembly: {e}");
                std::process::exit(1);
            }
        }
        Commands::Compile { input, output, emit_ir, print_ir, save_ir, ir_output, debug, bank_size, trace_spills, segmented_stack, no_preprocess, include_dirs, defines, trace, timings, target } => {
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
                segmented_stack,
                no_preprocess,
                include_dirs,
                defines,
                trace,
                timings,
                target,
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
    input_path: &std::path::Path,
    output_path: Option<&std::path::Path>,
    bank_size: u16,
    trace_spills: bool,
    segmented_stack: bool,
) -> Result<(), Box<dyn std::error::Error>> {
    let text = fs::read_to_string(input_path)?;
    let module = rcc_ir::parse_module(&text)?;
    let output = if let Some(path) = output_path {
        path.to_path_buf()
    } else {
        let mut path = input_path.to_path_buf();
        path.set_extension("asm");
        path
    };
    write_assembly(&module, &output, bank_size, trace_spills, segmented_stack)
}

fn finish_module(module: &Module, config: &CompileConfig) -> Result<(), Box<dyn std::error::Error>> {
    if module.target == rcc_ir::target::Target::Native && config.target != module.target {
        return Err("native IR cannot be retargeted to Ripple; recompile the source".into());
    }
    let selected = (module.target != config.target).then(|| {
        let mut selected = module.clone();
        selected.target = config.target;
        selected
    });
    let module = selected.as_ref().unwrap_or(module);
    if config.emit_ir || config.print_ir || config.save_ir {
        let _timer = StageTimer::new(config.timings, "ir_output");
        let ir_output = rcc_ir::emit_module(module);
        if config.emit_ir {
            print!("{ir_output}");
            return Ok(());
        }
        if config.print_ir {
            println!("\n=== IR Output ===");
            print!("{ir_output}");
            println!("=== End IR ===\n");
        }
        if config.save_ir {
            let ir_path = if let Some(path) = &config.ir_output_path {
                path.clone()
            } else {
                let mut path = config.input_path.clone();
                path.set_extension("ir");
                path
            };
            fs::write(&ir_path, &ir_output)?;
            println!("IR saved to: {}", ir_path.display());
        }
    }
    let output = if let Some(path) = &config.output_path {
        path.clone()
    } else {
        let mut path = config.input_path.clone();
        path.set_extension(if config.target == rcc_ir::target::Target::Native { "native" } else { "asm" });
        path
    };
    if config.target == rcc_ir::target::Target::Native {
        if config.segmented_stack { return Err("--segmented-stack is a Ripple calling convention; native uses its own logical stack".into()); }
        if config.bank_size != 64000 { return Err("native v1 data ABI requires --bank-size 64000".into()); }
        let _timer = StageTimer::new(config.timings, "native_codegen_link");
        rcc_native::compile_executable(module, &output).map_err(Into::into)
    } else {
        let _timer = StageTimer::new(config.timings, "ripple_lower_emit");
        write_assembly(module, &output, config.bank_size, config.trace_spills, config.segmented_stack)
    }
}

/// Opt-in diagnostics stay off stdout so --emit-ir remains machine-readable.
struct StageTimer {
    stage: &'static str,
    start: Option<std::time::Instant>,
}
impl StageTimer {
    fn new(enabled: bool, stage: &'static str) -> Self {
        Self { stage, start: enabled.then(std::time::Instant::now) }
    }
}
impl Drop for StageTimer {
    fn drop(&mut self) {
        if let Some(start) = self.start {
            eprintln!("rcc-timing {} {:.6}", self.stage, start.elapsed().as_secs_f64());
        }
    }
}

fn write_assembly(
    module: &Module,
    output: &std::path::Path,
    bank_size: u16,
    trace_spills: bool,
    segmented_stack: bool,
) -> Result<(), Box<dyn std::error::Error>> {
    let has_main = module.functions.iter().any(|func| func.name == "main");
    let options = LoweringOptions {
        bank_size,
        use_v2: true,
        trace_spills,
        segmented_stack,
    };
    match lower_module_to_assembly_with_options(module, options) {
        Ok(asm_instructions) => {
            println!("💕 Successfully lowered to assembly");
            let asm_text = rcc_codegen::emit::emit_complete_program(asm_instructions, has_main)?;
            fs::write(output, asm_text)?;
            println!("Assembly written to: {}", output.display());
            Ok(())
        }
        Err(e) => {
            eprintln!("Error: Failed to lower IR to assembly: {e}");
            eprintln!("Note: Code generation is still under development for complex features");
            Err(e.into())
        }
    }
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
    let _total = StageTimer::new(config.timings, "total");
    if config.input_path.extension().and_then(|ext| ext.to_str()) == Some("ir") {
        let timer = StageTimer::new(config.timings, "read_parse_ir");
        let text = fs::read_to_string(&config.input_path)?;
        let module = rcc_ir::parse_module(&text)?;
        drop(timer);
        if module.target != config.target {
            return Err(format!("IR target is {}; use --target {} or recompile the source", module.target.name(), module.target.name()).into());
        }
        if !config.emit_ir {
            println!(
                "Parsed IR module '{}' ({} functions, bank_size: {})",
                module.name,
                module.functions.len(),
                config.bank_size
            );
        }
        return finish_module(&module, &config);
    }

    // Don't print compilation message if we're just emitting IR
    if !config.emit_ir {
        println!("Compiling C99 file: {} (bank_size: {})", config.input_path.display(), config.bank_size);
    }
    
    // Read source file
    let mut source = {
        let _timer = StageTimer::new(config.timings, "read_source");
        fs::read_to_string(&config.input_path)?
    };
    
    // Preprocess unless explicitly disabled
    if !config.no_preprocess {
        let _timer = StageTimer::new(config.timings, "preprocess");
        let mut preprocessor = Preprocessor::new();
        
        // Add include directories
        for dir in &config.include_dirs {
            preprocessor.add_include_dir(dir.clone());
        }
        
        // Process defines
        for define in &config.defines {
            if let Some((name, value)) = define.split_once('=') {
                preprocessor.define(name.to_string(), Some(value.to_string()));
            } else {
                preprocessor.define(define.clone(), None);
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
    let parse_timer = StageTimer::new(config.timings, "parse");
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
    drop(parse_timer);
    
    // Save AST if tracing is enabled
    if config.trace {
        save_trace_file(&ast, &config.input_path, "ast.json")?;
        if !config.emit_ir {
            println!("Saved AST to trace file");
        }
    }
    
    // Perform semantic analysis
    let type_analyzer = {
        let _timer = StageTimer::new(config.timings, "semantic");
        let mut analyzer = SemanticAnalyzer::new();
        analyzer.analyze(&mut ast)?;
        analyzer.into_type_info()
    };
    
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
    let typed_ast = {
        let _timer = StageTimer::new(config.timings, "type");
        rcc_frontend::type_translation_unit(&ast, type_analyzer)
            .map_err(|e| CompilerError::internal_error(format!("Type error: {}", e)))?
    };
    
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
    let ir_timer = StageTimer::new(config.timings, "ir_generate");
    let generated = codegen.generate(&typed_ast);
    drop(ir_timer);
    match generated {
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
            
            finish_module(&ir_module, &config)
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
