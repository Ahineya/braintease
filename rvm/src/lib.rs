#[cfg(not(target_arch = "wasm32"))]
pub mod asm_formatter;
pub mod constants;
#[cfg(not(target_arch = "wasm32"))]
pub mod debug;
#[cfg(not(target_arch = "wasm32"))]
pub mod debugger_ui;
pub mod display_rgb565;
#[cfg(not(target_arch = "wasm32"))]
pub mod display_text40;
#[cfg(not(target_arch = "wasm32"))]
pub mod mode;
#[cfg(not(target_arch = "wasm32"))]
pub mod settings;
#[cfg(not(target_arch = "wasm32"))]
pub mod tui_debugger;
pub mod vm;

// Re-export commonly used types
#[cfg(not(target_arch = "wasm32"))]
pub use asm_formatter::{format_asm_line, format_instruction_spans, get_instruction_style};
pub use vm::{Instr, VM};
#[cfg(not(target_arch = "wasm32"))]
pub mod capture;
pub mod devices;
#[cfg_attr(target_arch = "wasm32", path = "web/input.rs")]
pub mod input;
pub mod input_events;
#[cfg_attr(target_arch = "wasm32", path = "web/network.rs")]
pub mod network;
#[cfg(not(target_arch = "wasm32"))]
pub mod runner;

#[cfg_attr(target_arch = "wasm32", path = "web/audio.rs")]
pub mod audio;

pub mod bulk;
pub mod float;

pub mod mmu;
#[cfg_attr(target_arch = "wasm32", path = "web/clock.rs")]
pub mod host_clock;

mod entropy;
pub mod terminal;

#[cfg_attr(target_arch = "wasm32", path = "web/crypto.rs")]
mod crypto;

#[cfg(target_arch = "wasm32")]
pub mod web;
