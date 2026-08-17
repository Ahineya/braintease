use crate::compiler::{compile_c_file, compile_bfm_file, ToolPaths};
use crate::config::{KnownFailure, RunConfig, TestCase, TestConfig, TestType};
use crate::reporter::{ProgressReporter, TestResult, TestStatus, TestSummary};
use anyhow::Result;
use rayon::prelude::*;
use std::path::Path;
use std::sync::Arc;
use std::time::Instant;

/// Test runner that executes tests
pub struct TestRunner {
    config: RunConfig,
    tools: Arc<ToolPaths>,
}

impl TestRunner {
    pub fn new(config: RunConfig, tools: ToolPaths) -> Self {
        Self {
            config,
            tools: Arc::new(tools),
        }
    }

    /// Run all tests
    pub fn run_all(&self, test_config: &TestConfig) -> Result<TestSummary> {
        let mut summary = TestSummary::default();

        // Separate tests by runtime usage and sort alphabetically
        let mut tests_without_runtime: Vec<_> = test_config
            .tests
            .iter()
            .filter(|t| !t.use_runtime)
            .collect();
        tests_without_runtime.sort_by_key(|t| t.file.file_stem().and_then(|s| s.to_str()).unwrap_or(""));
        
        let mut tests_with_runtime: Vec<_> = test_config
            .tests
            .iter()
            .filter(|t| t.use_runtime)
            .collect();
        tests_with_runtime.sort_by_key(|t| t.file.file_stem().and_then(|s| s.to_str()).unwrap_or(""));

        // Run tests without runtime
        if !tests_without_runtime.is_empty() {
            println!("\nTests without runtime (crt0 only):");
            println!("{}", "-".repeat(60));
            
            let results = self.run_test_batch(&tests_without_runtime);
            for result in results {
                result.print(self.config.verbose);
                summary.add(&result);
            }
        }

        // Run tests with runtime
        if !tests_with_runtime.is_empty() {
            println!("\nTests with runtime (crt0 + libruntime):");
            println!("{}", "-".repeat(60));
            
            let results = self.run_test_batch(&tests_with_runtime);
            for result in results {
                result.print(self.config.verbose);
                summary.add(&result);
            }
        }

        // Run known failures
        if !test_config.known_failures.is_empty() {
            println!("\nKnown failure tests:");
            println!("{}", "-".repeat(60));
            
            // Sort known failures alphabetically
            let mut sorted_failures = test_config.known_failures.clone();
            sorted_failures.sort_by_key(|f| f.file.file_stem().and_then(|s| s.to_str()).unwrap_or("").to_string());
            
            let results = self.run_known_failures(&sorted_failures);
            for result in results {
                result.print(self.config.verbose);
                summary.add(&result);
            }
        }

        Ok(summary)
    }

    /// Run a specific set of tests
    pub fn run_tests(&self, tests: &[String], test_config: &TestConfig) -> Result<TestSummary> {
        let mut summary = TestSummary::default();
        let mut test_cases = Vec::new();
        let mut not_found = Vec::new();

        // Find tests by name
        for test_name in tests {
            let name = test_name.strip_suffix(".c").unwrap_or(test_name);
            
            // Try to find in test config
            let mut found = false;
            
            if let Some(test) = test_config.tests.iter().find(|t| {
                t.file.file_stem()
                    .and_then(|s| s.to_str())
                    .map(|s| s == name)
                    .unwrap_or(false)
            }) {
                test_cases.push(test.clone());
                found = true;
            } else {
                // Try to find the file directly (check both .c and .bfm extensions)
                let possible_paths = [
                    format!("c-test/tests/{name}.c"),
                    format!("c-test/tests/{name}.bfm"),
                    format!("c-test/examples/{name}.c"),
                    format!("c-test/examples/{name}.bfm"),
                    format!("c-test/tests-known-failures/{name}.c"),
                    format!("c-test/tests-known-failures/{name}.bfm"),
                    format!("c-test/known-failures/{name}.c"),
                    format!("c-test/known-failures/{name}.bfm"),
                    format!("tests/{name}.c"),
                    format!("tests/{name}.bfm"),
                    format!("examples/{name}.c"),
                    format!("examples/{name}.bfm"),
                    format!("tests-known-failures/{name}.c"),
                    format!("tests-known-failures/{name}.bfm"),
                    format!("known-failures/{name}.c"),
                    format!("known-failures/{name}.bfm"),
                    // Also check for abfm-test subdirectory
                    format!("c-test/tests/abfm-test/{name}.bfm"),
                ];

                for path_str in &possible_paths {
                    let path = Path::new(path_str);
                    if path.exists() {
                        let test_type = if path.extension() == Some(std::ffi::OsStr::new("bfm")) {
                            TestType::Bfm
                        } else {
                            TestType::C
                        };
                        
                        test_cases.push(TestCase {
                            file: path.to_path_buf(),
                            expected: None,
                            use_runtime: test_type == TestType::C,
                            description: Some("Ad-hoc test".to_string()),
                            skipped: false,
                            test_type,
                            timeout_secs: None,
                        });
                        found = true;
                        break;
                    }
                }
            }
            
            if !found {
                not_found.push(test_name.clone());
            }
        }

        // Report any tests that weren't found
        if !not_found.is_empty() {
            use colored::*;
            eprintln!("{}", "ERROR: The following tests were not found:".red().bold());
            for name in &not_found {
                eprintln!("  {} {}", "✗".red(), name);
            }
            eprintln!();
            
            // Still run the tests that were found, but track that some were missing
            summary.not_found = not_found.len();
        }

        if test_cases.is_empty() {
            use colored::*;
            eprintln!("{}", "ERROR: No valid tests found to run!".red().bold());
            anyhow::bail!("No tests found matching the given names");
        }

        // Sort test cases alphabetically before running
        test_cases.sort_by_key(|t| t.file.file_stem().and_then(|s| s.to_str()).unwrap_or("").to_string());
        
        let results = self.run_test_batch(&test_cases.iter().collect::<Vec<_>>());
        for result in results {
            result.print(self.config.verbose);
            summary.add(&result);
        }

        Ok(summary)
    }

    /// Run a batch of tests (potentially in parallel)
    pub fn run_test_batch(&self, tests: &[&TestCase]) -> Vec<TestResult> {
        self.run_test_batch_with_tui(tests, false)
    }
    
    /// Run a batch of tests with TUI mode support
    pub fn run_test_batch_with_tui(&self, tests: &[&TestCase], tui_mode: bool) -> Vec<TestResult> {
        if self.config.parallel && tests.len() > 1 {
            // Create appropriate progress reporter based on mode
            let progress = if tui_mode {
                ProgressReporter::hidden()  // Hidden progress bar for TUI
            } else {
                ProgressReporter::new(tests.len())  // Normal progress bar for CLI
            };
            
            let results: Vec<TestResult> = tests
                .par_iter()
                .map(|test| {
                    let result = self.run_single_test(test);
                    progress.update(&test.file.display().to_string());
                    result
                })
                .collect();
            
            progress.finish();
            results
        } else {
            // Run tests sequentially
            tests.iter().map(|test| self.run_single_test(test)).collect()
        }
    }

    /// Run a single test
    pub fn run_single_test(&self, test: &TestCase) -> TestResult {
        let start = Instant::now();
        let test_name = test.file.display().to_string();
        
        // Check if test is skipped
        if test.skipped {
            return TestResult {
                name: test_name,
                status: TestStatus::Skipped,
                message: Some("Test marked as skipped".to_string()),
                actual_output: None,
                expected_output: test.expected.clone(),
                duration_ms: 0,
            };
        }
        
        // Fix the path - prepend c-test if needed
        let test_path = if test.file.is_relative() && !test.file.starts_with("c-test") {
            Path::new("c-test").join(&test.file)
        } else {
            test.file.clone()
        };

        // Check if file exists
        if !test_path.exists() {
            return TestResult {
                name: test_name,
                status: TestStatus::Skipped,
                message: Some("File not found".to_string()),
                actual_output: None,
                expected_output: None,
                duration_ms: 0,
            };
        }

        // Per-test timeout from metadata overrides the suite default.
        let mut config = self.config.clone();
        if let Some(timeout_secs) = test.timeout_secs {
            if timeout_secs > config.timeout_secs {
                config.timeout_secs = timeout_secs;
            }
        }

        // Compile and run based on test type
        let result = match test.test_type {
            TestType::Bfm => compile_bfm_file(
                &test_path,
                &self.tools,
                &config,
            ),
            TestType::C => compile_c_file(
                &test_path,
                &self.tools,
                &config,
                test.use_runtime,
            ),
        };

        let duration_ms = start.elapsed().as_millis() as u64;

        match result {
            Ok(compilation_result) => {
                if !compilation_result.success {
                    TestResult {
                        name: test_name,
                        status: if compilation_result.error_message
                            .as_ref()
                            .map(|m| m.contains("Timeout"))
                            .unwrap_or(false)
                        {
                            TestStatus::Timeout
                        } else {
                            TestStatus::CompilationError
                        },
                        message: compilation_result.error_message,
                        actual_output: Some(compilation_result.output),
                        expected_output: test.expected.clone(),
                        duration_ms,
                    }
                } else if let Some(expected) = &test.expected {
                    // Compare output
                    if compilation_result.output == *expected {
                        TestResult {
                            name: test_name,
                            status: if compilation_result.has_provenance_warning {
                                TestStatus::PassedWithWarnings
                            } else {
                                TestStatus::Passed
                            },
                            message: if compilation_result.has_provenance_warning {
                                Some("Pointer provenance unknown".to_string())
                            } else {
                                None
                            },
                            actual_output: Some(compilation_result.output),
                            expected_output: Some(expected.clone()),
                            duration_ms,
                        }
                    } else {
                        TestResult {
                            name: test_name,
                            status: TestStatus::Failed,
                            message: Some("Output mismatch".to_string()),
                            actual_output: Some(compilation_result.output),
                            expected_output: Some(expected.clone()),
                            duration_ms,
                        }
                    }
                } else {
                    // No expected output - just show what we got
                    TestResult {
                        name: test_name,
                        status: if compilation_result.has_provenance_warning {
                            TestStatus::PassedWithWarnings
                        } else {
                            TestStatus::Passed
                        },
                        message: None,
                        actual_output: Some(compilation_result.output),
                        expected_output: None,
                        duration_ms,
                    }
                }
            }
            Err(e) => TestResult {
                name: test_name,
                status: TestStatus::CompilationError,
                message: Some(format!("Error: {e}")),
                actual_output: None,
                expected_output: test.expected.clone(),
                duration_ms,
            },
        }
    }

    /// Run known failure tests
    fn run_known_failures(&self, failures: &[KnownFailure]) -> Vec<TestResult> {
        failures
            .iter()
            .map(|failure| {
                let test_name = failure.file.display().to_string();
                
                // Fix the path - prepend c-test if needed
                let test_path = if failure.file.is_relative() && !failure.file.starts_with("c-test") {
                    Path::new("c-test").join(&failure.file)
                } else {
                    failure.file.clone()
                };
                
                if !test_path.exists() {
                    return TestResult {
                        name: test_name,
                        status: TestStatus::Skipped,
                        message: Some("File not found".to_string()),
                        actual_output: None,
                        expected_output: None,
                        duration_ms: 0,
                    };
                }

                // Try to compile - we expect it to fail
                // Determine test type from file extension
                let test_type = if test_path.extension() == Some(std::ffi::OsStr::new("bfm")) {
                    TestType::Bfm
                } else {
                    TestType::C
                };
                
                let result = match test_type {
                    TestType::Bfm => compile_bfm_file(
                        &test_path,
                        &self.tools,
                        &self.config,
                    ),
                    TestType::C => compile_c_file(
                        &test_path,
                        &self.tools,
                        &self.config,
                        true, // assume runtime usage for C
                    ),
                };

                match result {
                    Ok(compilation_result) => {
                        if compilation_result.success && !compilation_result.has_provenance_warning {
                            // Unexpected pass
                            TestResult {
                                name: test_name,
                                status: TestStatus::UnexpectedPass,
                                message: Some("Expected to fail but passed".to_string()),
                                actual_output: Some(compilation_result.output),
                                expected_output: None,
                                duration_ms: 0,
                            }
                        } else {
                            // Expected failure or has warnings
                            TestResult {
                                name: test_name,
                                status: TestStatus::KnownFailure,
                                message: failure.description.clone(),
                                actual_output: None,
                                expected_output: None,
                                duration_ms: 0,
                            }
                        }
                    }
                    Err(_) => {
                        // Expected failure
                        TestResult {
                            name: test_name,
                            status: TestStatus::KnownFailure,
                            message: failure.description.clone(),
                            actual_output: None,
                            expected_output: None,
                            duration_ms: 0,
                        }
                    }
                }
            })
            .collect()
    }
}

/// Clean up build artifacts
pub fn cleanup_build_dir(build_dir: &Path) -> Result<usize> {
    let patterns = [
        "*.asm",
        "*.ir",
        "*.pobj",
        "*.bfm",
        "*.bin",
        "*_expanded.bf",
        "*.disassembly.asm",
    ];

    let mut total = 0;
    for pattern in &patterns {
        let full_pattern = build_dir.join(pattern);
        if let Some(pattern_str) = full_pattern.to_str() {
            total += crate::command::cleanup_pattern(pattern_str)?;
        }
    }

    Ok(total)
}