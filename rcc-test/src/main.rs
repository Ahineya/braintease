use anyhow::{Context, Result};
use clap::Parser;
use colored::*;
use rcc_test::cli::{Cli, Command};
use rcc_test::compiler::{build_runtime, compile_c_to_binary, compile_bfm_to_binary, ToolPaths};
use rcc_test::config::{self, RunConfig, TestType};
use rcc_test::runner::{cleanup_build_dir, TestRunner};
use rcc_test::tui::runner::TuiRunner;
use std::path::{Path, PathBuf};
use std::process;

fn main() {
    if let Err(e) = run() {
        eprintln!("{}: {}", "Error".red().bold(), e);
        process::exit(1);
    }
}

fn run() -> Result<()> {
    let cli = Cli::parse();

    // Determine project root
    let project_root = if let Some(root) = cli.project_root.clone() {
        root
    } else {
        detect_project_root()?
    };

    // Create tool paths
    let tools = ToolPaths::new(&project_root, &cli.build_dir);

    // Ensure build directory exists
    std::fs::create_dir_all(&cli.build_dir)?;

    // Handle subcommands
    match cli.command {
        Some(Command::Clean) => {
            println!("Cleaning build directory...");
            let count = cleanup_build_dir(&cli.build_dir)?;
            println!("Removed {count} files");
            return Ok(());
        }

        Some(Command::BuildRuntime) => {
            println!("Building runtime library (bank_size: {})...", cli.bank_size);
            build_runtime(&tools, cli.bank_size)?;
            println!("{}", "Runtime built successfully".green());
            return Ok(());
        }

        Some(Command::Add { file, expected, no_runtime, description }) => {
            add_test(&cli.tests_file, file, expected, !no_runtime, description)?;
            return Ok(());
        }

        Some(Command::List { names_only, include_failures, categories }) => {
            if categories {
                list_categories(&cli.tests_file)?;
            } else {
                list_tests(&cli.tests_file, names_only, include_failures)?;
            }
            return Ok(());
        }

        Some(Command::Stats) => {
            show_stats(&cli.tests_file)?;
            return Ok(());
        }
        
        Some(Command::Rename { ref old_name, ref new_name }) => {
            rename_test(&cli.tests_file, old_name, new_name)?;
            return Ok(());
        }
        
        Some(Command::Check { verbose }) => {
            check_untracked_tests(&cli.tests_file, verbose)?;
            return Ok(());
        }

        Some(Command::Debug { ref test }) => {
            debug_test(test, &cli, &tools)?;
            return Ok(());
        }

        Some(Command::Tui { ref filter, ref category, ref test_name }) => {
            run_tui(&cli, &tools, filter.clone(), category.clone(), test_name.clone())?;
            return Ok(());
        }

        Some(Command::Run { ref programs, ref filter, ref frequency, visual, bank_size, ref disk }) => {
            // Override CLI disk path if Run command has its own disk flag
            let mut cli_with_disk = cli.clone();
            if let Some(disk_path) = disk {
                cli_with_disk.disk = Some(disk_path.clone());
            }
            
            if !programs.is_empty() {
                // Run as programs without test expectations
                let run_bank_size = bank_size.unwrap_or(cli.bank_size);
                for program in programs {
                    exec_program(program, &cli_with_disk, &tools, frequency.clone(), visual, run_bank_size)?;
                }
            } else {
                // Run as test suite with frequency if provided
                run_tests_with_frequency(&cli_with_disk, &tools, filter.clone(), frequency.clone())?;
            }
        }
        None => {
            // Check if debug mode is requested with specific tests
            if cli.debug && !cli.tests.is_empty() {
                // Run each test in debug mode
                for test in &cli.tests {
                    debug_test(test, &cli, &tools)?;
                }
            } else if let Some(ref category) = cli.category {
                // Run tests by category
                run_tests_by_category(&cli, &tools, category)?;
            } else {
                // Default to run command with no filter
                run_tests(&cli, &tools, None)?;
            }
        }
    }

    Ok(())
}

fn detect_project_root() -> Result<PathBuf> {
    let current_dir = std::env::current_dir()?;
    
    // Check if we're in the project root (contains c-test directory)
    if current_dir.join("c-test").exists() {
        return Ok(current_dir);
    }
    
    // Check if we're in c-test directory
    if current_dir.file_name() == Some(std::ffi::OsStr::new("c-test")) {
        if let Some(parent) = current_dir.parent() {
            return Ok(parent.to_path_buf());
        }
    }
    
    // Check if we're in rcc-test directory
    if current_dir.file_name() == Some(std::ffi::OsStr::new("rcc-test")) {
        if let Some(parent) = current_dir.parent() {
            return Ok(parent.to_path_buf());
        }
    }
    
    // Try parent directory
    if let Some(parent) = current_dir.parent() {
        if parent.join("c-test").exists() {
            return Ok(parent.to_path_buf());
        }
    }
    
    anyhow::bail!("Could not detect project root. Please run from project root or use --project-root")
}

fn run_tests(cli: &Cli, tools: &ToolPaths, filter: Option<String>) -> Result<()> {
    run_tests_with_frequency(cli, tools, filter, None)
}

fn run_tests_by_category(cli: &Cli, tools: &ToolPaths, category: &str) -> Result<()> {
    // Build runtime first
    println!("Building runtime library (bank_size: {})...", cli.bank_size);
    build_runtime(tools, cli.bank_size)?;
    
    // Load test configuration
    let test_config = config::load_tests(&cli.tests_file)?;
    
    // Filter tests by category
    let category_path = normalize_category_to_path(category);
    let filtered_tests: Vec<String> = test_config
        .tests
        .iter()
        .filter(|t| {
            let test_path = t.file.to_string_lossy();
            // Check if the test path contains the category path
            test_path.contains(&category_path)
        })
        .map(|t| {
            t.file.file_stem()
                .and_then(|s| s.to_str())
                .unwrap_or("")
                .to_string()
        })
        .collect();
    
    if filtered_tests.is_empty() {
        println!("No tests found in category: {category}");
        return Ok(());
    }
    
    println!("Running {} tests in category '{}'", filtered_tests.len(), category);
    
    // Create run configuration
    let run_config = RunConfig {
        backend: cli.backend.to_backend(),
        timeout_secs: cli.timeout,
        bank_size: cli.bank_size,
        verbose: cli.verbose,
        no_cleanup: true,  // Always preserve files
        parallel: !cli.no_parallel,
        debug_mode: cli.debug,
        frequency: None,
        disk_path: cli.disk.clone().or_else(|| RunConfig::default().disk_path),
    };
    
    // Create test runner
    let runner = TestRunner::new(run_config, tools.clone());
    
    // Run the filtered tests
    let summary = runner.run_tests(&filtered_tests, &test_config)?;
    
    // Print summary
    summary.print();
    
    // Skip cleanup - always preserve files
    // (Previously cleaned up based on cli.no_cleanup flag)
    
    process::exit(summary.exit_code())
}

fn normalize_category_to_path(category: &str) -> String {
    // Convert category names like "core" or "memory/arrays" to path segments
    // that match the directory structure under c-test/tests/
    let normalized = category.to_lowercase().replace('-', "_");
    
    // For nested categories, ensure proper path separators
    if normalized.contains('/') {
        format!("tests/{normalized}")
    } else {
        format!("tests/{normalized}")
    }
}

fn run_tests_with_frequency(cli: &Cli, tools: &ToolPaths, filter: Option<String>, frequency: Option<String>) -> Result<()> {
    // Build runtime first
    println!("Building runtime library (bank_size: {})...", cli.bank_size);
    build_runtime(tools, cli.bank_size)?;
    
    // Load test configuration
    let test_config = config::load_tests(&cli.tests_file)?;
    
    // Create run configuration
    let run_config = RunConfig {
        backend: cli.backend.to_backend(),
        timeout_secs: cli.timeout,
        bank_size: cli.bank_size,
        verbose: cli.verbose,
        no_cleanup: true,  // Always preserve files
        parallel: !cli.no_parallel,
        debug_mode: cli.debug,
        frequency,
        disk_path: cli.disk.clone().or_else(|| RunConfig::default().disk_path),
    };
    
    // Create test runner
    let runner = TestRunner::new(run_config, tools.clone());
    
    // Run tests
    let summary = if !cli.tests.is_empty() {
        // Run specific tests
        runner.run_tests(&cli.tests, &test_config)?
    } else if let Some(pattern) = filter {
        // Filter tests by pattern
        let filtered_tests: Vec<String> = test_config
            .tests
            .iter()
            .filter(|t| {
                t.file.to_str()
                    .map(|s| s.contains(&pattern))
                    .unwrap_or(false)
            })
            .map(|t| {
                t.file.file_stem()
                    .and_then(|s| s.to_str())
                    .unwrap_or("")
                    .to_string()
            })
            .collect();
        
        if filtered_tests.is_empty() {
            println!("No tests match filter: {pattern}");
            return Ok(());
        }
        
        println!("Running {} tests matching '{}'", filtered_tests.len(), pattern);
        runner.run_tests(&filtered_tests, &test_config)?
    } else {
        // Run all tests
        runner.run_all(&test_config)?
    };
    
    // Print summary
    summary.print();
    
    // Skip cleanup - always preserve files
    // (Previously cleaned up based on cli.no_cleanup flag)
    
    process::exit(summary.exit_code())
}

fn add_test(
    tests_file: &Path,
    file: PathBuf,
    expected: Option<String>,
    use_runtime: bool,
    description: Option<String>,
) -> Result<()> {
    // Load existing config or create new
    let mut config = if tests_file.exists() {
        config::load_tests(tests_file)?
    } else {
        config::TestConfig {
            tests: Vec::new(),
            known_failures: Vec::new(),
        }
    };
    
    // Process escape sequences in expected output
    let expected = expected.map(|s| {
        s.replace("\\n", "\n")
            .replace("\\t", "\t")
            .replace("\\r", "\r")
            .replace("\\\\", "\\")
    });
    
    // Normalize the file path and determine if it's a known failure
    let (file, is_known_failure) = if file.is_relative() {
        if file.starts_with("known-failures") {
            (file, true)
        } else if file.starts_with("examples") {
            // Examples are tracked like known failures (no expected output)
            (file, true)
        } else if file.starts_with("tests") {
            (file, false)
        } else {
            // Try to find the file in the test directories
            let test_file = find_test_file_in_hierarchy(&file)?;
            (test_file, false)
        }
    } else {
        // For absolute paths, check if it contains known-failures or examples
        let is_failure = file.to_str()
            .map(|s| s.contains("known-failures") || s.contains("examples"))
            .unwrap_or(false);
        (file, is_failure)
    };
    
    // For BFM files, never use runtime
    let use_runtime = if file.extension() == Some(std::ffi::OsStr::new("bfm")) {
        false
    } else {
        use_runtime
    };
    
    let description_for_save = description.clone();
    let test_type = if file.extension() == Some(std::ffi::OsStr::new("bfm")) {
        config::TestType::Bfm
    } else {
        config::TestType::C
    };
    let save_as_known_failure = is_known_failure || expected.is_none();

    // Add to appropriate section
    let is_new = if is_known_failure || expected.is_none() {
        // Add to known failures
        if config.known_failures.iter().any(|f| f.file == file) {
            // Update existing
            if let Some(failure) = config.known_failures.iter_mut().find(|f| f.file == file) {
                if description.is_some() {
                    failure.description = description;
                }
                false
            } else {
                false
            }
        } else {
            config.known_failures.push(rcc_test::config::KnownFailure {
                file: file.clone(),
                description,
            });
            true
        }
    } else {
        // Add to regular tests
        config::add_test(
            &mut config,
            file.clone(),
            expected.clone(),
            use_runtime,
            description,
        )
    };
    
    // Write only this test's metadata. Rewriting the whole suite reorders
    // every .meta.json and drops fields the in-memory config did not round-trip.
    config::save_test_metadata(
        &file,
        &expected,
        use_runtime,
        &description_for_save,
        save_as_known_failure,
        false,
        test_type,
        None,
    )?;
    
    if is_new {
        println!("Added new test: {}", file.display());
    } else {
        println!("Updated existing test: {}", file.display());
    }
    
    if let Some(exp) = expected {
        println!("Expected output: {exp:?}");
    }
    
    Ok(())
}

fn list_tests(tests_file: &Path, names_only: bool, include_failures: bool) -> Result<()> {
    let config = config::load_tests(tests_file)?;
    
    if !names_only {
        println!("Available tests:");
        println!("{}", "-".repeat(60));
    }
    
    for test in &config.tests {
        if names_only {
            if let Some(stem) = test.file.file_stem() {
                println!("{}", stem.to_string_lossy());
            }
        } else {
            println!(
                "{} (runtime: {}, expected: {}, skipped: {})",
                test.file.display(),
                if test.use_runtime { "yes" } else { "no" },
                if test.expected.is_some() { "defined" } else { "none" },
                if test.skipped { "yes" } else { "no" }
            );
            if let Some(desc) = &test.description {
                println!("  {}", desc.dimmed());
            }
        }
    }
    
    if include_failures {
        if !names_only {
            println!("\nKnown failures:");
            println!("{}", "-".repeat(60));
        }
        
        for failure in &config.known_failures {
            if names_only {
                if let Some(stem) = failure.file.file_stem() {
                    println!("{}", stem.to_string_lossy());
                }
            } else {
                println!("{}", failure.file.display());
                if let Some(desc) = &failure.description {
                    println!("  {}", desc.dimmed());
                }
            }
        }
    }
    
    Ok(())
}

fn list_categories(tests_file: &Path) -> Result<()> {
    use std::collections::BTreeMap;
    
    let config = config::load_tests(tests_file)?;
    
    // Collect tests into categories
    let mut categories: BTreeMap<String, usize> = BTreeMap::new();
    
    // Process regular tests
    for test in &config.tests {
        let category = get_category_from_path(&test.file);
        *categories.entry(category).or_insert(0) += 1;
    }
    
    // Process known failures
    for failure in &config.known_failures {
        let category = get_category_from_path(&failure.file);
        let category_with_prefix = format!("{category} (known failures)");
        *categories.entry(category_with_prefix).or_insert(0) += 1;
    }
    
    // Print categories
    println!("Available test categories:");
    println!("{}", "=".repeat(60));
    
    let mut total_tests = 0;
    for (category, count) in &categories {
        println!("{category:<40} {count} tests");
        total_tests += count;
    }
    
    println!("{}", "=".repeat(60));
    println!("Total: {} categories, {} tests", categories.len(), total_tests);
    
    // Print usage hint
    println!("\nUsage: rct -c <category> to run tests in a specific category");
    println!("Example: rct -c core/typedef");
    
    Ok(())
}

fn get_category_from_path(path: &PathBuf) -> String {
    let path_str = path.to_string_lossy();
    let parts: Vec<&str> = path_str.split('/').collect();
    
    // Find where test directories start
    let mut start_idx = None;
    for (i, part) in parts.iter().enumerate() {
        if *part == "tests" || *part == "tests-runtime" || 
           *part == "tests-known-failures" || *part == "known-failures" || 
           *part == "examples" {
            start_idx = Some(i);
            break;
        }
    }
    
    if let Some(idx) = start_idx {
        let root = parts[idx];
        
        match root {
            "tests-runtime" => "runtime".to_string(),
            "examples" => "examples".to_string(),
            "tests-known-failures" | "known-failures" => {
                // Check for subdirectory
                if parts.len() > idx + 2 {
                    format!("known-failures/{}", parts[idx + 1])
                } else {
                    "known-failures".to_string()
                }
            },
            "tests" => {
                // Build category from path components (excluding the filename)
                let mut category_parts = Vec::new();
                for i in (idx + 1)..(parts.len() - 1) {
                    category_parts.push(parts[i]);
                }
                
                if category_parts.is_empty() {
                    "uncategorized".to_string()
                } else {
                    category_parts.join("/")
                }
            },
            _ => "uncategorized".to_string()
        }
    } else {
        "uncategorized".to_string()
    }
}

fn show_stats(tests_file: &Path) -> Result<()> {
    let config = config::load_tests(tests_file)?;
    
    println!("Test Suite Statistics");
    println!("{}", "=".repeat(60));
    println!("Total tests:         {}", config.tests.len());
    
    let with_runtime = config.tests.iter().filter(|t| t.use_runtime).count();
    let without_runtime = config.tests.len() - with_runtime;
    
    println!("With runtime:        {with_runtime}");
    println!("Without runtime:     {without_runtime}");
    
    let with_expected = config.tests.iter().filter(|t| t.expected.is_some()).count();
    println!("With expected output: {with_expected}");
    
    println!("Known failures:      {}", config.known_failures.len());
    
    // Calculate total lines of expected output
    let total_expected_lines: usize = config
        .tests
        .iter()
        .filter_map(|t| t.expected.as_ref())
        .map(|e| e.lines().count())
        .sum();
    
    println!("Total expected lines: {total_expected_lines}");
    
    Ok(())
}

fn debug_test(test_name: &str, cli: &Cli, tools: &ToolPaths) -> Result<()> {
    // Build runtime first
    println!("Building runtime library...");
    build_runtime(tools, cli.bank_size)?;
    
    // Find the test file
    let test_path = find_test_file(test_name, &cli.tests_file)?;
    
    // Fix the path - prepend c-test if needed
    let actual_test_path = if test_path.is_relative() && !test_path.starts_with("c-test") {
        Path::new("c-test").join(&test_path)
    } else {
        test_path.clone()
    };
    
    println!("Compiling: {}", actual_test_path.display());
    
    // Compile the test using the appropriate function based on file type
    let bin_file = if actual_test_path.extension() == Some(std::ffi::OsStr::new("bfm")) {
        compile_bfm_to_binary(
            &actual_test_path,
            tools,
            cli.bank_size,
            30,   // timeout_secs
        )?
    } else {
        compile_c_to_binary(
            &actual_test_path,
            tools,
            cli.bank_size,
            true, // use_runtime for C files
            30,   // timeout_secs
        )?
    };
    
    println!("{}", format!("Successfully built {}", bin_file.display()).green());
    
    // Run with debugger
    println!("\nStarting debugger...");
    println!("{}", "-".repeat(60));
    
    let mut cmd = std::process::Command::new(&tools.rvm);
    cmd.arg(&bin_file);
    cmd.arg("-t");
    
    // Add disk path if provided (or use default test.img)
    let disk_path = cli.disk.clone().or_else(|| RunConfig::default().disk_path);
    if let Some(disk) = disk_path {
        cmd.arg("--disk").arg(disk);
    }
    
    let status = cmd.status()?;
    
    if !status.success() {
        anyhow::bail!("Debugger exited with error");
    }
    
    Ok(())
}

fn rename_test(tests_file: &Path, old_name: &str, new_name: &str) -> Result<()> {
    // Load test configuration
    let mut config = config::load_tests(tests_file)?;
    
    // Normalize names - strip .c extension if present
    let old_name = old_name.strip_suffix(".c").unwrap_or(old_name);
    let new_name = new_name.strip_suffix(".c").unwrap_or(new_name);
    
    // Find the test to rename
    let mut found_test = None;
    let mut found_index = None;
    
    for (idx, test) in config.tests.iter().enumerate() {
        if let Some(stem) = test.file.file_stem() {
            if stem.to_str() == Some(old_name) || test.file.to_str() == Some(old_name) {
                found_test = Some(test.clone());
                found_index = Some(idx);
                break;
            }
        }
    }
    
    // Check in known failures if not found in tests
    let mut is_known_failure = false;
    if found_test.is_none() {
        for (idx, failure) in config.known_failures.iter().enumerate() {
            if let Some(stem) = failure.file.file_stem() {
                if stem.to_str() == Some(old_name) || failure.file.to_str() == Some(old_name) {
                    // Convert to TestCase for uniform handling
                    let test_type = if failure.file.extension() == Some(std::ffi::OsStr::new("bfm")) {
                        TestType::Bfm
                    } else {
                        TestType::C
                    };
                    found_test = Some(config::TestCase {
                        file: failure.file.clone(),
                        expected: None,
                        use_runtime: test_type == TestType::C,
                        description: failure.description.clone(),
                        skipped: false,
                        test_type,
                        timeout_secs: None,
                    });
                    found_index = Some(idx);
                    is_known_failure = true;
                    break;
                }
            }
        }
    }
    
    if let Some(test) = found_test {
        // Determine old and new paths
        let old_path = if test.file.is_relative() && !test.file.starts_with("c-test") {
            PathBuf::from("c-test").join(&test.file)
        } else {
            test.file.clone()
        };
        
        // Construct new path preserving directory structure
        let new_file_path = if test.file.starts_with("tests-known-failures") {
            PathBuf::from("tests-known-failures").join(format!("{new_name}.c"))
        } else if test.file.starts_with("tests") {
            PathBuf::from("tests").join(format!("{new_name}.c"))
        } else {
            PathBuf::from(format!("{new_name}.c"))
        };
        
        let new_path = if new_file_path.is_relative() && !new_file_path.starts_with("c-test") {
            PathBuf::from("c-test").join(&new_file_path)
        } else {
            new_file_path.clone()
        };
        
        // Check if old file exists
        if !old_path.exists() {
            anyhow::bail!("Source file {} does not exist", old_path.display());
        }
        
        // Check if new file already exists
        if new_path.exists() {
            anyhow::bail!("Target file {} already exists", new_path.display());
        }
        
        // Rename the actual file
        std::fs::rename(&old_path, &new_path)
            .context(format!("Failed to rename file from {} to {}", old_path.display(), new_path.display()))?;
        
        // Also rename the .meta.json file
        let old_meta_path = old_path.with_extension("meta.json");
        let new_meta_path = new_path.with_extension("meta.json");
        if old_meta_path.exists() {
            std::fs::rename(&old_meta_path, &new_meta_path)
                .context(format!("Failed to rename metadata file from {} to {}", old_meta_path.display(), new_meta_path.display()))?;
        }
        
        // Update the configuration
        if is_known_failure {
            if let Some(idx) = found_index {
                config.known_failures[idx].file = new_file_path;
            }
        } else if let Some(idx) = found_index {
            config.tests[idx].file = new_file_path;
        }
        
        // Save the updated configuration
        config::save_tests(&config, tests_file)?;
        
        println!("✓ Renamed test '{old_name}' to '{new_name}'");
        println!("  File: {} -> {}", old_path.display(), new_path.display());
        
        Ok(())
    } else {
        anyhow::bail!("Test '{}' not found in tests.json", old_name)
    }
}

fn check_untracked_tests(tests_file: &Path, verbose: bool) -> Result<()> {
    use std::collections::HashSet;
    
    // Load existing tests to get tracked files
    let config = config::load_tests(tests_file)?;
    
    // Create a set of tracked file paths (normalized)
    let mut tracked_files = HashSet::new();
    
    // Add regular tests
    for test in &config.tests {
        // Normalize the path - add c-test prefix if needed
        let normalized_path = if test.file.is_relative() && !test.file.starts_with("c-test") {
            PathBuf::from("c-test").join(&test.file)
        } else {
            test.file.clone()
        };
        tracked_files.insert(normalized_path);
    }
    
    // Add known failures
    for failure in &config.known_failures {
        let normalized_path = if failure.file.is_relative() && !failure.file.starts_with("c-test") {
            PathBuf::from("c-test").join(&failure.file)
        } else {
            failure.file.clone()
        };
        tracked_files.insert(normalized_path);
    }
    
    // Scan test directories for .c files recursively
    let untracked_files = TestFinder::find_untracked_c_files(&tracked_files)?;
    
    // Report results
    if untracked_files.is_empty() {
        println!("{}", "✓ All test files are tracked in tests.json".green());
    } else {
        println!("{}", format!("Found {} untracked test files:", untracked_files.len()).yellow().bold());
        println!();
        
        for file in &untracked_files {
            // Get relative path for cleaner display
            let display_path = if file.starts_with("c-test/") {
                file.strip_prefix("c-test/").unwrap_or(file)
            } else {
                file.as_path()
            };
            
            println!("  {}", display_path.display());
            
            if verbose {
                // Try to read first few lines to guess what the test does
                if let Ok(content) = std::fs::read_to_string(file) {
                    let lines: Vec<&str> = content.lines().take(10).collect();
                    
                    // Look for main function or comments
                    for line in lines {
                        if line.contains("main(") || line.starts_with("//") || line.starts_with("/*") {
                            println!("    {}", line.trim().dimmed());
                        }
                    }
                }
            }
        }
        
        println!();
        println!("To add these tests, use:");
        println!("  {} add <file> <expected_output>", "rct".cyan());
        println!();
        println!("Example:");
        if let Some(first) = untracked_files.first() {
            let path = if first.starts_with("c-test/") {
                first.strip_prefix("c-test/").unwrap_or(first)
            } else {
                first.as_path()
            };
            println!("  {} add {} \"Y\\n\"", "rct".cyan(), path.display());
        }
    }
    
    Ok(())
}

fn exec_program(program_name: &str, cli: &Cli, tools: &ToolPaths, frequency: Option<String>, visual: bool, bank_size: usize) -> Result<()> {
    // Build runtime first
    println!("Building runtime library (bank_size: {})...", bank_size);
    build_runtime(tools, bank_size)?;
    
    // Find the program file
    let program_path = find_program_file(program_name)?;
    
    // Fix the path - prepend c-test if needed
    let actual_path = if program_path.is_relative() && !program_path.starts_with("c-test") {
        Path::new("c-test").join(&program_path)
    } else {
        program_path.clone()
    };
    
    println!("Compiling: {}", actual_path.display());
    
    // Compile the program using the appropriate function based on file type
    let bin_file = if actual_path.extension() == Some(std::ffi::OsStr::new("bfm")) {
        compile_bfm_to_binary(
            &actual_path,
            tools,
            bank_size,
            30,   // timeout_secs
        )?
    } else {
        compile_c_to_binary(
            &actual_path,
            tools,
            bank_size,
            true, // use_runtime (always with runtime for C examples)
            30,   // timeout_secs
        )?
    };
    
    println!("{}", format!("Successfully built {}", bin_file.display()).green());
    
    // Run the program
    println!("\nRunning: {} {}", tools.rvm.display(), bin_file.display());
    println!("{}", "-".repeat(60));
    
    if cli.debug {
        // Run with debugger
        let mut cmd = std::process::Command::new(&tools.rvm);
        cmd.arg(&bin_file);
        cmd.arg("-t");
        
        // Add disk path if provided (or use default test.img)
        let disk_path = cli.disk.clone().or_else(|| RunConfig::default().disk_path);
        if let Some(disk) = disk_path {
            cmd.arg("--disk").arg(disk);
        }
        
        let status = cmd.status()?;
        
        if !status.success() {
            anyhow::bail!("Program exited with error");
        }
    } else {
        // Run normally and just pass through output
        let mut cmd = std::process::Command::new(&tools.rvm);
        cmd.arg(&bin_file);
        
        // Add frequency parameter if provided
        if let Some(freq) = frequency {
            cmd.arg("--frequency").arg(freq);
        }
        
        // Add visual flag if requested
        if visual {
            cmd.arg("--visual");
        }
        
        // Add disk path if provided (or use default test.img)
        let disk_path = cli.disk.clone().or_else(|| RunConfig::default().disk_path);
        if let Some(disk) = disk_path {
            cmd.arg("--disk").arg(disk);
        }
        
        let status = cmd.status()?;
        
        if !status.success() {
            anyhow::bail!("Program exited with error");
        }
    }
    
    Ok(())
}

fn run_tui(cli: &Cli, tools: &ToolPaths, filter: Option<String>, category: Option<String>, test_name: Option<String>) -> Result<()> {
    // Load test configuration
    let test_config = config::load_tests(&cli.tests_file)?;
    
    // Create and run TUI
    let mut tui_runner = TuiRunner::new(
        test_config,
        tools.clone(),
        cli.bank_size,
        cli.timeout,
    );
    
    // Apply initial filter if provided
    if let Some(_filter) = filter {
        // TODO: Set initial filter in TUI app
    }
    
    // Set initial category if provided
    if let Some(_category) = category {
        // TODO: Set initial category in TUI app
    }
    
    // Jump to specific test if provided
    if let Some(test_name) = test_name {
        tui_runner.jump_to_test(&test_name);
    }
    
    tui_runner.run()?;
    
    Ok(())
}

/// Central test file finder that searches recursively in c-test directory
struct TestFinder;

impl TestFinder {
    const TEST_ROOT: &'static str = "c-test";
    const BUILD_DIR: &'static str = "build";
    
    /// Find a C or BFM file by name in the test directories
    fn find_c_file(name: &str) -> Result<PathBuf> {
        // First check if it's a direct path
        let direct_path = Path::new(name);
        if direct_path.exists() && direct_path.is_file() {
            return Ok(direct_path.to_path_buf());
        }
        
        // Normalize the name
        let name = name.strip_suffix(".c")
            .or_else(|| name.strip_suffix(".bfm"))
            .unwrap_or(name);
        
        // Try to find both .c and .bfm files
        let c_filename = format!("{name}.c");
        let bfm_filename = format!("{name}.bfm");
        
        // Search recursively for .c file first
        if let Ok(found) = Self::find_file_recursive(Path::new(Self::TEST_ROOT), &c_filename) {
            // Return relative path from c-test/
            if let Ok(relative) = found.strip_prefix(Self::TEST_ROOT) {
                return Ok(relative.to_path_buf());
            }
            return Ok(found);
        }
        
        // Then search for .bfm file
        if let Ok(found) = Self::find_file_recursive(Path::new(Self::TEST_ROOT), &bfm_filename) {
            // Return relative path from c-test/
            if let Ok(relative) = found.strip_prefix(Self::TEST_ROOT) {
                return Ok(relative.to_path_buf());
            }
            return Ok(found);
        }
        
        anyhow::bail!("File '{}' not found", name)
    }
    
    /// Recursively find a specific file in a directory tree
    fn find_file_recursive(dir: &Path, filename: &str) -> Result<PathBuf> {
        if !dir.exists() || !dir.is_dir() {
            return Err(anyhow::anyhow!("Directory does not exist"));
        }
        
        for entry in std::fs::read_dir(dir)? {
            let entry = entry?;
            let path = entry.path();
            
            if path.is_dir() {
                // Skip build directory to avoid false positives
                if path.file_name() == Some(std::ffi::OsStr::new(Self::BUILD_DIR)) {
                    continue;
                }
                // Recursively search subdirectories
                if let Ok(found) = Self::find_file_recursive(&path, filename) {
                    return Ok(found);
                }
            } else if path.is_file() {
                // Check if this is the file we're looking for
                if path.file_name() == Some(std::ffi::OsStr::new(filename)) {
                    return Ok(path);
                }
            }
        }
        
        Err(anyhow::anyhow!("File not found"))
    }
    
    /// Find all untracked C and BFM files in the test directories
    fn find_untracked_c_files(tracked_files: &std::collections::HashSet<PathBuf>) -> Result<Vec<PathBuf>> {
        let mut untracked = Vec::new();
        Self::find_untracked_recursive(Path::new(Self::TEST_ROOT), tracked_files, &mut untracked)?;
        untracked.sort();
        Ok(untracked)
    }
    
    fn find_untracked_recursive(
        dir: &Path,
        tracked_files: &std::collections::HashSet<PathBuf>,
        untracked_files: &mut Vec<PathBuf>,
    ) -> Result<()> {
        if !dir.exists() || !dir.is_dir() {
            return Ok(());
        }
        
        for entry in std::fs::read_dir(dir)? {
            let entry = entry?;
            let path = entry.path();
            
            if path.is_dir() {
                // Skip build directory
                if path.file_name() == Some(std::ffi::OsStr::new(Self::BUILD_DIR)) {
                    continue;
                }
                // Recursively search subdirectories
                Self::find_untracked_recursive(&path, tracked_files, untracked_files)?;
            } else if path.extension() == Some(std::ffi::OsStr::new("c")) || 
                      path.extension() == Some(std::ffi::OsStr::new("bfm")) {
                // Check if this C or BFM file is tracked
                if !tracked_files.contains(&path) {
                    untracked_files.push(path);
                }
            }
        }
        
        Ok(())
    }
}

// Now update the functions to use TestFinder
fn find_program_file(program_name: &str) -> Result<PathBuf> {
    TestFinder::find_c_file(program_name)
}

fn find_test_file(test_name: &str, tests_file: &Path) -> Result<PathBuf> {
    // Try to load config and find test
    if let Ok(config) = config::load_tests(tests_file) {
        if let Some(test) = config::find_test(&config, test_name) {
            return Ok(test.file.clone());
        }
    }
    
    // Fall back to file search
    TestFinder::find_c_file(test_name)
}

fn find_test_file_in_hierarchy(file: &PathBuf) -> Result<PathBuf> {
    let name = file.file_stem()
        .and_then(|s| s.to_str())
        .ok_or_else(|| anyhow::anyhow!("Invalid file name"))?;
    
    TestFinder::find_c_file(name)
}
