use ratatui::prelude::{Color, Modifier, Span, Style};
use ripple_asm::{Register, Opcode};
use crate::vm::Instr;
use std::collections::HashMap;

/// Formats an assembly instruction as colored spans for TUI display
pub fn format_instruction_spans(
    instr: &Instr,
    debug_symbols: &HashMap<usize, String>,
    show_debug_symbols: bool,
) -> Vec<Span<'static>> {
    let mut spans = vec![];
    
    // Get opcode name and style
    let opcode_str = opcode_name(instr.opcode);
    let opcode_style = get_instruction_style(instr);
    
    // Convert u8 to Opcode enum for cleaner matching
    let opcode = Opcode::from_u8(instr.opcode);
    
    match opcode {
        Some(Opcode::Halt) => {
            spans.push(Span::styled("HALT", Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)));
        },
        Some(Opcode::Nop) => {
            spans.push(Span::styled("NOP", opcode_style));
        },
        Some(Opcode::Add) | Some(Opcode::Sub) | Some(Opcode::And) | Some(Opcode::Or) | 
        Some(Opcode::Xor) | Some(Opcode::Sll) | Some(Opcode::Srl) | Some(Opcode::Slt) | 
        Some(Opcode::Sltu) | Some(Opcode::Mul) | Some(Opcode::Div) | Some(Opcode::Mod) |
        Some(Opcode::Loadc) | Some(Opcode::Storc) => {
            // R-type: op rd, rs, rt
            spans.push(Span::styled(format!("{opcode_str:<6} "), opcode_style));
            spans.push(Span::styled(register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0)))); // Dark green
            spans.push(Span::raw(", "));
            spans.push(Span::styled(register_name(instr.word2 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
            spans.push(Span::raw(", "));
            spans.push(Span::styled(register_name(instr.word3 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
        },
        Some(Opcode::Addi) | Some(Opcode::Andi) | Some(Opcode::Ori) | Some(Opcode::Xori) | 
        Some(Opcode::Slli) | Some(Opcode::Srli) | Some(Opcode::Muli) | Some(Opcode::Divi) | Some(Opcode::Modi) => {
            // I-type with immediate: op rd, rs, imm
            spans.push(Span::styled(format!("{opcode_str:<6} "), opcode_style));
            spans.push(Span::styled(register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
            spans.push(Span::raw(", "));
            spans.push(Span::styled(register_name(instr.word2 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
            spans.push(Span::raw(", "));
            spans.push(Span::styled(format!("0x{:X}", instr.word3), Style::default().fg(Color::Rgb(255, 140, 0)))); // Orange
        },
        Some(Opcode::Li) => {
            // LI: li rd, imm
            spans.push(Span::styled(format!("{opcode_str:<6} "), opcode_style));
            spans.push(Span::styled(register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
            spans.push(Span::raw(", "));
            spans.push(Span::styled(format!("0x{:X}", instr.word2), Style::default().fg(Color::Rgb(255, 140, 0)))); // Orange
        },
        Some(Opcode::Load) | Some(Opcode::Store) => {
            // LOAD/STORE: op r, bank, addr
            spans.push(Span::styled(format!("{opcode_str:<6} "), opcode_style));
            spans.push(Span::styled(register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
            spans.push(Span::raw(", "));
            
            // Bank operand (could be register or immediate)
            if instr.word2 < 32 {
                spans.push(Span::styled(register_name(instr.word2 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
            } else {
                spans.push(Span::styled(format!("0x{:X}", instr.word2), Style::default().fg(Color::Rgb(255, 140, 0))));
            }
            spans.push(Span::raw(", "));
            
            // Address operand (could be register or immediate)
            if instr.word3 < 32 {
                spans.push(Span::styled(register_name(instr.word3 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
            } else {
                spans.push(Span::styled(format!("0x{:X}", instr.word3), Style::default().fg(Color::Rgb(255, 140, 0))));
            }
        },
        Some(Opcode::Jal) => {
            // JAL: jal rd, addr
            spans.push(Span::styled(format!("{opcode_str:<6} "), opcode_style));
            spans.push(Span::styled(register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
            spans.push(Span::raw(", "));
            
            // Check if we have a function name for this address (only if debug symbols are enabled)
            let target_addr = instr.word3 as usize;
            if show_debug_symbols {
                if let Some(func_name) = debug_symbols.get(&target_addr) {
                    // Show function name in bright cyan
                    spans.push(Span::styled(func_name.clone(), Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)));
                    // Also show the address in parentheses
                    spans.push(Span::styled(format!(" (0x{target_addr:04X})"), Style::default().fg(Color::DarkGray)));
                } else {
                    // No debug info, just show address
                    spans.push(Span::styled(format!("0x{target_addr:04X}"), Style::default().fg(Color::Rgb(255, 140, 0))));
                }
            } else {
                // Debug symbols disabled, just show address
                spans.push(Span::styled(format!("0x{target_addr:04X}"), Style::default().fg(Color::Rgb(255, 140, 0))));
            }
        },
        Some(Opcode::Jalr) => {
            // JALR: jalr rd, rs
            spans.push(Span::styled(format!("{opcode_str:<6} "), opcode_style));
            spans.push(Span::styled(register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
            spans.push(Span::raw(", "));
            spans.push(Span::styled(register_name(instr.word3 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
        },
        Some(Opcode::Beq) | Some(Opcode::Bne) | Some(Opcode::Blt) | Some(Opcode::Bge) => {
            // Branch: beq/bne/blt/bge rs, rt, offset
            spans.push(Span::styled(format!("{opcode_str:<6} "), opcode_style));
            spans.push(Span::styled(register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
            spans.push(Span::raw(", "));
            spans.push(Span::styled(register_name(instr.word2 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
            spans.push(Span::raw(", "));
            let offset = instr.word3 as i16;
            spans.push(Span::styled(format!("{offset}"), Style::default().fg(Color::Rgb(255, 140, 0))));
        },
        Some(Opcode::Brk) => {
            // BRK
            spans.push(Span::styled("BRK", Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)));
        },
        None => {
            spans.push(Span::styled(format!("??? 0x{:02X}", instr.opcode), Style::default().fg(Color::DarkGray)));
        },
    }
    
    spans
}

/// Gets the style for an instruction based on its type
pub fn get_instruction_style(instr: &Instr) -> Style {
    let opcode = Opcode::from_u8(instr.opcode);
    
    match opcode {
        Some(Opcode::Halt) => {
            Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)
        },
        Some(Opcode::Brk) => Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
        Some(Opcode::Jal) | Some(Opcode::Jalr) | Some(Opcode::Beq) | 
        Some(Opcode::Bne) | Some(Opcode::Blt) | Some(Opcode::Bge) => {
            // Jump and branch instructions
            Style::default().fg(Color::Yellow)
        },
        Some(Opcode::Load) | Some(Opcode::Store) | Some(Opcode::Loadc) | Some(Opcode::Storc) => {
            // Memory operations
            Style::default().fg(Color::Blue)
        },
        Some(Opcode::Add) | Some(Opcode::Sub) | Some(Opcode::And) | Some(Opcode::Or) | 
        Some(Opcode::Xor) | Some(Opcode::Sll) | Some(Opcode::Srl) | Some(Opcode::Slt) | 
        Some(Opcode::Sltu) | Some(Opcode::Mul) | Some(Opcode::Div) | Some(Opcode::Mod) => {
            // Arithmetic and logical operations
            Style::default().fg(Color::Rgb(0, 200, 0))
        },
        _ => Style::default().fg(Color::White),
    }
}

/// Formats a single line of assembly for colored display
pub fn format_asm_line(line: &str) -> Vec<Span<'static>> {
    let line_owned = line.to_string();
    let trimmed = line_owned.trim();
    
    // Check for label lines (end with ':')
    if trimmed.ends_with(':') {
        return vec![Span::styled(
            line_owned.clone(),
            Style::default().fg(Color::Magenta).add_modifier(Modifier::BOLD)
        )];
    }
    
    // Split into parts for instruction lines
    let parts: Vec<String> = trimmed.split_whitespace().map(|s| s.to_string()).collect();
    if parts.is_empty() {
        return vec![Span::raw(line_owned.clone())];
    }
    
    let mut spans = vec![];
    
    // Check for comment lines
    if parts[0].starts_with(';') || parts[0].starts_with('#') {
        return vec![Span::styled(
            line_owned.clone(),
            Style::default().fg(Color::DarkGray)
        )];
    }
    
    // Get the opcode (first word)
    let opcode_str = parts[0].to_lowercase();
    
    // Style the opcode based on its type
    let opcode_style = match opcode_str.as_str() {
        // Control flow
        "jal" | "jalr" | "beq" | "bne" | "blt" | "bge" | "brk" | "halt" => {
            Style::default().fg(Color::Yellow)
        },
        // Memory operations
        "load" | "store" | "li" | "loadc" | "storc" => {
            Style::default().fg(Color::Blue)
        },
        // Arithmetic and logical
        "add" | "addi" | "sub" | "and" | "andi" | "or" | "ori" | 
        "xor" | "xori" | "sll" | "slli" | "srl" | "srli" | "slt" | "sltu" |
        "mul" | "muli" | "div" | "divi" | "mod" | "modi" => {
            Style::default().fg(Color::Rgb(0, 200, 0))
        },
        // Special
        "nop" => Style::default().fg(Color::DarkGray),
        _ => Style::default().fg(Color::White),
    };
    
    // Add indentation if present
    let indent_len = line_owned.len() - line_owned.trim_start().len();
    if indent_len > 0 {
        spans.push(Span::raw(line_owned.chars().take(indent_len).collect::<String>()));
    }
    
    // Add styled opcode
    spans.push(Span::styled(format!("{:<6}", parts[0]), opcode_style));
    
    // Add the rest of the line
    if parts.len() > 1 {
        // Join the operands
        let operands = parts[1..].join(" ");
        
        // Simple coloring for operands: registers in green, numbers in orange
        for (i, part) in operands.split(',').enumerate() {
            if i > 0 {
                spans.push(Span::raw(","));
            }
            
            let trimmed_part = part.trim().to_string();
            
            // Check if it's a register
            if trimmed_part.starts_with('r') || trimmed_part.starts_with('R') ||
               ["pc", "pcb", "ra", "rab"].contains(&trimmed_part.to_lowercase().as_str()) {
                spans.push(Span::styled(
                    format!(" {trimmed_part}"),
                    Style::default().fg(Color::Rgb(0, 200, 0))
                ));
            }
            // Check if it's a number (hex or decimal)
            else if trimmed_part.starts_with("0x") || trimmed_part.starts_with("0X") ||
                    trimmed_part.parse::<i32>().is_ok() {
                spans.push(Span::styled(
                    format!(" {trimmed_part}"),
                    Style::default().fg(Color::Rgb(255, 140, 0))
                ));
            }
            // Check for labels/symbols
            else if !trimmed_part.is_empty() {
                spans.push(Span::styled(
                    format!(" {trimmed_part}"),
                    Style::default().fg(Color::Cyan)
                ));
            }
        }
    }
    
    spans
}

fn opcode_name(opcode: u8) -> &'static str {
    match Opcode::from_u8(opcode) {
        Some(op) => op.to_str(),
        None => "???",
    }
}

fn register_name(reg: u8) -> &'static str {
    match Register::from_u8(reg) {
        Some(r) => r.to_str(),
        None => "??",
    }
}