mod cli;
use clap::Parser;
use cli::Cli;
use colored::*;
use constants::DEFAULT_MEMORY_SIZE;
use crossterm::{cursor, style::ResetColor, terminal, ExecutableCommand};
use debug::Debugger;
use rvm::{constants, debug, devices, display_rgb565, runner, tui_debugger, vm};
use std::fs;
use std::io::{self, Write};
use std::process;
use std::thread;
use std::time::Duration;
use vm::VM;

/// Install signal handlers to ensure terminal cleanup on exit
fn install_signal_handlers() {
    use signal_hook::{consts::SIGINT, consts::SIGTERM, iterator::Signals};
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
    let file_path = cli.binary_file.clone();
    if cli.clock == runner::ClockPolicy::Stepped && !cli.control_stdin {
        return Err("--clock stepped requires --control-stdin".into());
    }
    if cli.control_stdin && cli.clock != runner::ClockPolicy::Stepped {
        return Err("--control-stdin requires --clock stepped".into());
    }
    let options = runner::RunOptions {
        clock: cli.clock,
        screenshot: cli.screenshot.clone(),
        raw_frame: cli.raw_frame.clone(),
        expect_frame: cli.expect_frame.clone(),
        capture_frame: cli.capture_frame,
        max_instructions: cli.max_instructions,
        max_ticks: cli.max_ticks,
        timeout: cli.timeout.map(Duration::from_secs),
        frequency,
        deterministic: cli.headless
            || cli.input_script.is_some()
            || cli.screenshot.is_some()
            || cli.raw_frame.is_some(),
        control: cli.control_stdin.then(runner::stdin_control),
    };

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

    vm.devices = devices::DeviceState::new(cli.tick_hz)?;
    if let Some(path) = cli.input_script.as_ref() {
        vm.devices
            .set_script(devices::InputScript::from_path(path)?)?;
    }
    vm.host_input_enabled = !options.deterministic && !cli.control_stdin;

    // Set verbose mode if requested
    vm.verbose = verbose;
    vm.jit_enabled = cli.jit;

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

    let network_setting = cli
        .net
        .as_ref()
        .map(|p| p.as_os_str().to_owned())
        .or_else(|| std::env::var_os("RIPPLE_NET"));
    vm.devices
        .network
        .configure_host(network_setting.as_deref())?;

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
        println!(
            "Starting execution at PC={}, PCB={}",
            vm.registers[1], vm.registers[2]
        );
        println!();
    }

    let mut report = runner::RunReport {
        report_version: 1,
        success: false,
        stop_reason: String::new(),
        instructions: 0,
        tick: 0,
        milliseconds: 0,
        tick_hz: cli.tick_hz,
        clock: cli.clock,
        frame: 0,
        width: None,
        height: None,
        bank_size: vm.bank_size,
        memory_cells: vm.memory.len(),
        seed: rng_seed.unwrap_or(0x12345678),
        binary_sha256: runner::sha256_file(&file_path)?,
        disk_sha256: cli.disk.as_deref().map(runner::sha256_file).transpose()?,
        input_script_sha256: cli
            .input_script
            .as_deref()
            .map(runner::sha256_file)
            .transpose()?,
        input_overflow: false,
        jit: None,
    };
    let mut guest_exit: Option<i32> = None;
    let vm_moved_to_arc = if visual_mode {
        vm.rgb565_display
            .get_or_insert_with(display_rgb565::RGB565Display::new);
        let display_state = vm.rgb565_display.as_ref().unwrap().get_state();
        let worker_state = display_state.clone();
        let report_path = cli.report.clone();
        let capture_requested = options.screenshot.is_some() || options.raw_frame.is_some();
        let vm_thread = thread::spawn(move || {
            let result = runner::run(&mut vm, &options);
            report.finish(&vm, &result);
            if capture_requested || result.is_err() {
                worker_state.lock().unwrap().request_shutdown();
            }
            if let Some(path) = report_path {
                report.write(&path)?;
            }
            result?;
            Ok::<i32, String>(if matches!(vm.state, vm::VMState::Halted) {
                (vm.registers[ripple_asm::Register::Rv0 as usize] & 0xff) as i32
            } else {
                0
            })
        });
        let window_result = display_rgb565::run_rgb565_display(display_state.clone());
        display_state.lock().unwrap().request_shutdown();
        let status = vm_thread.join().map_err(|_| "VM worker panicked")??;
        window_result?;
        if propagate_exit_code && status != 0 {
            guest_exit = Some(status);
        }
        true
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
        vm.debug_mode = true; // Enable debug mode in VM
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
                }
                "c" if matches!(vm.state, vm::VMState::Breakpoint) => {
                    // Continue from breakpoint.rs
                    vm.state = vm::VMState::Running;
                    // Continue stepping
                }
                _ => {
                    // Step one instruction
                    if let Err(e) = vm.step() {
                        eprintln!("{}: {}", "Runtime error".bright_red().bold(), e);
                        process::exit(1);
                    }

                    // Check for output
                    let output = vm.get_output();
                    if !output.is_empty() {
                        println!(
                            "\n{}: {}",
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
        let result = runner::run(&mut vm, &options);
        report.finish(&vm, &result);
        if let Some(path) = cli.report.as_ref() {
            report.write(path)?;
        }
        result?;
        if propagate_exit_code && matches!(vm.state, vm::VMState::Halted) {
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
