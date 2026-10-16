pub mod asm_formatter;
pub mod constants;
pub mod debug;
pub mod debugger_ui;
pub mod display_rgb565;
pub mod display_text40;
pub mod mode;
pub mod settings;
pub mod tui_debugger;
pub mod vm;

// Re-export commonly used types
pub use asm_formatter::{format_asm_line, format_instruction_spans, get_instruction_style};
pub use vm::{Instr, VM};
pub mod capture;
pub mod devices;
pub mod input;
pub mod input_events;
pub mod network;
pub mod runner;

pub mod audio;

pub mod bulk;

pub mod mmu;
