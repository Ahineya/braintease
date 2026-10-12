pub mod vm;
pub mod tui_debugger;
pub mod debugger_ui;
pub mod mode;
pub mod settings;
pub mod constants;
pub mod debug;
pub mod asm_formatter;
pub mod display_rgb565;
pub mod display_text40;

// Re-export commonly used types
pub use vm::{VM, Instr};
pub use asm_formatter::{format_asm_line, format_instruction_spans, get_instruction_style};
pub mod devices;
pub mod capture;
pub mod runner;
