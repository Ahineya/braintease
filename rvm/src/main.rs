mod constants;
mod debug;
mod tui_debugger;
mod vm;
mod debugger_ui;
mod mode;
mod settings;
mod display_rgb565;
mod cli;

use std::fs;
use std::io::{self, Write};
use std::process;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use vm::VM;
use constants::DEFAULT_MEMORY_SIZE;
use debug::Debugger;
use colored::*;
use crossterm::{terminal, cursor, style::ResetColor, ExecutableCommand};
use clap::Parser;
use cli::Cli;

/// Install signal handlers to ensure terminal cleanup on exit
fn install_signal_handlers() {
    use signal_hook::{consts::SIGTERM, consts::SIGINT, iterator::Signals};
    use std::thread;
    
    // Try to install signal handlers, but don't fail if it doesn't work
    // (e.g., on some platforms or in some environments)
    if let Ok(mut signals) = Signals::new(&[SIGINT, SIGTERM]) {
        thread::spawn(move || {
            for _ in signals.forever() {
                // Restore terminal before exiting
                let _ = terminal::disable_raw_mode();
                let _ = io::stderr().execute(cursor::Show);
                let _ = io::stderr().execute(terminal::LeaveAlternateScreen);
                let _ = io::stderr().execute(ResetColor);
                let _ = io::stderr().flush();
                
                // Exit the process
                process::exit(130); // 128 + SIGINT(2) = 130
            }
        });
    }
}


fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Install terminal cleanup hooks
    vm::install_terminal_cleanup_hook();
    install_signal_handlers();
    
    // Parse command line arguments
    let cli = Cli::parse();
    
    let bank_size = cli.bank_size;
    let memory_size = cli.memory;
    let frequency = cli.parse_frequency();
    let rng_seed = cli.parse_seed();
    let input_text = cli.input.clone();
    let debug_mode = cli.debug;
    let tui_mode = cli.tui;
    let verbose = cli.verbose;
    let visual_mode = cli.visual;
    let propagate_exit_code = cli.exit_code;
    let disk_path = cli.disk.clone();
    let file_path = cli.binary_file;
    
    // Read the binary file
    let binary = fs::read(&file_path).unwrap_or_else(|e| {
        eprintln!("Error reading file '{}': {}", file_path.display(), e);
        process::exit(1);
    });
    
    // Create and initialize the VM
    let mut vm = if let Some(mem_size) = memory_size {
        VM::with_options(bank_size, mem_size, disk_path)
    } else {
        VM::with_options(bank_size, DEFAULT_MEMORY_SIZE, disk_path)
    };
    
    // Set RNG seed if specified
    if let Some(seed) = rng_seed {
        vm.set_rng_seed(seed);
        if verbose {
            println!("RNG seed set to: 0x{seed:08X}");
        }
    }
    
    // Set verbose mode if requested
    vm.verbose = verbose;
    
    // Pre-populate input buffer if provided
    if let Some(input) = input_text {
        // Replace escape sequences with actual characters
        let processed_input = input
            .replace("\\n", "\n")
            .replace("\\r", "\r")
            .replace("\\t", "\t");
        vm.push_input_string(&processed_input);
        if verbose {
            println!("Pre-populated input buffer with: {:?}", processed_input);
        }
    }
    
    // Load the binary
    if let Err(e) = vm.load_binary(&binary) {
        eprintln!("Error loading binary: {e}");
        process::exit(1);
    }
    
    if verbose {
        println!("Loading binary from {}...", file_path.display());
        println!("Bank size: {bank_size}");
        println!("Memory size: {} words", vm.memory.len());
        if let Some(ref disk) = cli.disk {
            println!("Disk image: {}", disk.display());
        }
    }
    
    if verbose {
        println!("Loaded {} instructions", vm.instructions.len());
        println!("Starting execution at PC={}, PCB={}", vm.registers[1], vm.registers[2]);
        println!();
    }
    
    // Run the VM
    let mut guest_exit: Option<i32> = None;
    let vm_moved_to_arc = if visual_mode {
        // Visual mode: run VM in background thread, display on main thread
        eprintln!("Starting in visual mode...");
        
        // Initialize a default RGB565 display that will be configured when the program runs
        if vm.rgb565_display.is_none() {
            vm.rgb565_display = Some(display_rgb565::RGB565Display::new());
        }
        
        // Get the display state
        let display_state = vm.rgb565_display.as_ref().unwrap().get_state();
        
        // Run VM in a background thread
        let vm = Arc::new(Mutex::new(vm));
        let vm_clone = Arc::clone(&vm);
        let frequency_clone = frequency;
        
        let vm_thread = thread::spawn(move || {
            let mut vm = vm_clone.lock().unwrap();
            
            // Run with frequency limiting if specified
            if let Some(freq) = frequency_clone {
                if let Err(e) = run_with_frequency(&mut vm, freq) {
                    eprintln!("Runtime error: {}", e);
                }
            } else {
                if let Err(e) = vm.run() {
                    eprintln!("Runtime error: {}", e);
                }
            }
            
            eprintln!("VM execution completed. Close the window to exit.");
            // Don't shutdown display immediately - let user close the window
        });
        
        // Run display on main thread
        if let Err(e) = display_rgb565::run_rgb565_display(display_state) {
            eprintln!("Display error: {}", e);
        }
        
        // Wait for VM thread to finish
        let _ = vm_thread.join();
        true // VM was moved to Arc
    } else if tui_mode {
        // Use the TUI debugger_ui
        vm.debug_mode = true;
        let mut tui = tui_debugger::TuiDebugger::new();
        if let Err(e) = tui.run(&mut vm) {
            eprintln!("TUI error: {e}");
            process::exit(1);
        }
        false // VM not moved to Arc
    } else if debug_mode {
        vm.debug_mode = true;  // Enable debug mode in VM
        Debugger::print_welcome();
        
        let stdin = io::stdin();
        let mut input = String::new();
        
        // Show initial state
        let debugger = Debugger::new();
        debugger.print_state(&vm);
        
        loop {
            
            // Check VM state
            match vm.state {
                vm::VMState::Halted => {
                    println!("\n{}", "Program halted".bright_red().bold());
                    break;
                }
                vm::VMState::Breakpoint => {
                    println!("\n{}", ">>> Breakpoint hit <<<".bright_yellow().bold());
                    println!("{}", "Press 'c' to continue, Enter to step".bright_black());
                }
                _ => {}
            }
            
            // Wait for input with colored prompt
            print!("{} ", ">".bright_green().bold());
            io::stdout().flush().unwrap();
            input.clear();
            stdin.read_line(&mut input).unwrap();
            
            match input.trim() {
                "q" => break,
                "r" => {
                    // Run to completion
                    if let Err(e) = vm.run() {
                        eprintln!("Runtime error: {e}");
                        process::exit(1);
                    }
                    break;
                },
                "c" if matches!(vm.state, vm::VMState::Breakpoint) => {
                    // Continue from breakpoint.rs
                    vm.state = vm::VMState::Running;
                    // Continue stepping
                },
                _ => {
                    // Step one instruction
                    if let Err(e) = vm.step() {
                        eprintln!("{}: {}", "Runtime error".bright_red().bold(), e);
                        process::exit(1);
                    }
                    
                    // Check for output
                    let output = vm.get_output();
                    if !output.is_empty() {
                        println!("\n{}: {}", 
                            "Output".bright_cyan().bold(),
                            String::from_utf8_lossy(&output)
                        );
                    }
                    
                    // Show new state
                    debugger.print_state(&vm);
                }
            }
        }
        false // VM not moved to Arc
    } else {
        // Run normally with optional frequency limiting
        if let Some(freq) = frequency {
            if verbose {
                println!("Running at {freq} Hz");
            }
            run_with_frequency(&mut vm, freq)?;
        } else if let Err(e) = vm.run() {
            eprintln!("Runtime error: {e}");
            process::exit(1);
        }
        if propagate_exit_code {
            let status = (vm.registers[ripple_asm::Register::Rv0 as usize] as i32) & 0xff;
            if status != 0 {
                guest_exit = Some(status);
            }
        }
        false // VM not moved to Arc
    };
    
    // Output is now printed in real-time during execution
    if verbose && !vm_moved_to_arc {
        println!();
        println!("Execution completed");
    }
    
    // Restore the host terminal only if we actually took it over.
    // Unconditional LeaveAlternateScreen writes CSI sequences to stderr
    // and pollutes captured test output.
    if tui_mode || debug_mode || visual_mode {
        let _ = terminal::disable_raw_mode();
        let _ = io::stderr().execute(cursor::Show);
        let _ = io::stderr().execute(terminal::LeaveAlternateScreen);
        let _ = io::stderr().execute(ResetColor);
        let _ = io::stderr().flush();
    }

    if let Some(status) = guest_exit {
        process::exit(status);
    }
    
    Ok(())
}


/// Run VM with frequency limiting
fn run_with_frequency(vm: &mut VM, frequency: u64) -> Result<(), String> {
    // Target 60 FPS for smooth animation
    const TARGET_FPS: u64 = 60;
    const NANOS_PER_SECOND: u64 = 1_000_000_000;
    
    // Calculate instructions per frame
    let instructions_per_frame = frequency / TARGET_FPS;
    let frame_duration = Duration::from_nanos(NANOS_PER_SECOND / TARGET_FPS);
    
    let mut last_frame_time = Instant::now();
    let mut instructions_in_frame = 0;
    
    while matches!(vm.state, vm::VMState::Running) {
        // Execute one instruction
        vm.step()?;
        instructions_in_frame += 1;
        
        // Check if we've executed enough instructions for this frame
        if instructions_in_frame >= instructions_per_frame {
            // Wait for the rest of the frame duration
            let elapsed = last_frame_time.elapsed();
            if elapsed < frame_duration {
                std::thread::sleep(frame_duration - elapsed);
            }
            
            // Reset for next frame
            last_frame_time = Instant::now();
            instructions_in_frame = 0;
        }
        
        // Stop if we hit a breakpoint in debug mode
        if matches!(vm.state, vm::VMState::Breakpoint) {
            break;
        }
    }
    
    Ok(())
}