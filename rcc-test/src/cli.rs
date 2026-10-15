use clap::{Parser, Subcommand, ValueEnum};
use std::path::PathBuf;

#[derive(Parser, Debug, Clone)]
#[command(
    name = "rct",
    about = "Test runner for the Ripple C compiler",
    long_about = "rct - Ripple C Compiler Test runner\n\nA fast, parallel test runner for the Ripple C compiler with support for both Brainfuck and RVM backends.",
    version,
    author
)]
pub struct Cli {
    /// Test names to run (without path or .c extension)
    #[arg(value_name = "TEST")]
    pub tests: Vec<String>,

    /// Run tests in a specific category (e.g., "core", "memory/arrays")
    #[arg(short = 'c', long = "category")]
    pub category: Option<String>,

    /// Execution backend
    #[arg(short, long, default_value = "rvm")]
    pub backend: BackendArg,

    /// Timeout in seconds for test execution
    #[arg(short, long, default_value = "2")]
    pub timeout: u64,

    /// Bank size for assembler
    #[arg(long, default_value = "64000")]
    pub bank_size: usize,

    /// Show output from test programs as they run
    #[arg(short, long)]
    pub verbose: bool,

    /// Don't clean up generated files after tests
    #[arg(long)]
    pub no_cleanup: bool,

    /// Disable parallel test execution
    #[arg(long)]
    pub no_parallel: bool,

    /// Use debug mode (RVM with -t flag)
    #[arg(short, long)]
    pub debug: bool,
    
    /// Path to disk image file for storage (passed to RVM)
    #[arg(long)]
    pub disk: Option<PathBuf>,

    /// Path to test directory (deprecated, kept for compatibility)
    #[arg(long, default_value = "c-test", hide = true)]
    pub tests_file: PathBuf,

    /// Build directory for artifacts
    #[arg(long, default_value = "c-test/build")]
    pub build_dir: PathBuf,

    /// Project root directory
    #[arg(long)]
    pub project_root: Option<PathBuf>,

    #[command(subcommand)]
    pub command: Option<Command>,
}

#[derive(Subcommand, Debug, Clone)]
pub enum Command {
    /// Run tests (default if no command specified) or execute programs
    Run {
        /// Programs to run (if provided, runs as programs not tests)
        programs: Vec<String>,
        
        /// Run tests matching a pattern (only for test mode)
        #[arg(short, long)]
        filter: Option<String>,
        
        /// Set virtual CPU frequency (e.g., 1MHz, 500KHz, 60Hz)
        #[arg(long)]
        frequency: Option<String>,
        
        /// Enable visual display mode for graphics programs
        #[arg(long)]
        visual: bool,
        
        /// Bank size for assembler
        #[arg(long)]
        bank_size: Option<usize>,
        
        /// Path to disk image file for storage (passed to RVM)
        #[arg(long)]
        disk: Option<PathBuf>,
    },
    
    /// Add a new test (creates .meta.json file)
    Add {
        /// Test file path
        file: PathBuf,
        
        /// Expected output (use \n for newlines)
        expected: Option<String>,
        
        /// Test doesn't use runtime
        #[arg(long)]
        no_runtime: bool,
        
        /// Test description
        #[arg(short, long)]
        description: Option<String>,
    },
    
    /// Clean build directory
    Clean,
    
    /// List all available tests
    List {
        /// Show only test names (no details)
        #[arg(short, long)]
        names_only: bool,
        
        /// Include known failures
        #[arg(short, long)]
        include_failures: bool,
        
        /// List categories instead of tests
        #[arg(short = 'c', long)]
        categories: bool,
    },
    
    /// Build and run a single test interactively
    Debug {
        /// Test name or file path
        test: String,
    },
    
    /// Build runtime library
    BuildRuntime,
    
    /// Show statistics about the test suite
    Stats,
    
    /// Rename a test (updates both .meta.json and .c file)
    Rename {
        /// Current test name or path
        old_name: String,
        
        /// New test name or path
        new_name: String,
    },
    
    /// Check for test files not added to the test suite
    Check {
        /// Show detailed output
        #[arg(short, long)]
        verbose: bool,
    },
    
    /// Launch interactive TUI for test management
    Tui {
        /// Initial filter pattern
        #[arg(short, long)]
        filter: Option<String>,
        
        /// Start with a specific category selected
        #[arg(short, long)]
        category: Option<String>,
        
        /// Test name to scroll to initially (without path or .c extension)
        #[arg(value_name = "TEST")]
        test_name: Option<String>,
    },
}

#[derive(Debug, Clone, Copy, ValueEnum)]
pub enum BackendArg {
    Bf,
    Rvm,
    Native,
}

impl BackendArg {
    pub fn to_backend(&self) -> crate::config::Backend {
        match self {
            BackendArg::Bf => crate::config::Backend::Brainfuck,
            BackendArg::Rvm => crate::config::Backend::Rvm,
            BackendArg::Native => crate::config::Backend::Native,
        }
    }
}