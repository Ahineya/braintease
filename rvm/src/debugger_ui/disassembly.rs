use ratatui::Frame;
use ratatui::layout::Rect;
use ratatui::prelude::{Color, Line, Modifier, Span, Style};
use ratatui::widgets::{Block, Borders, List, ListItem};
use ripple_asm::{Register, Opcode};
use crate::tui_debugger::TuiDebugger;
use crate::vm::{Instr, VM};

impl TuiDebugger {
    pub(crate) fn draw_disassembly(&self, frame: &mut Frame, area: Rect, vm: &VM) {
        let current_pc = vm.registers[Register::Pc as usize] as usize;
        let current_pcb = vm.registers[Register::Pcb as usize] as usize;
        let current_idx = current_pcb * vm.bank_size as usize + current_pc;

        let mut items = Vec::new();
        let visible_lines = area.height.saturating_sub(2) as usize;

        // Calculate visible range
        let start_idx = self.disasm_scroll;
        let end_idx = (start_idx + visible_lines).min(vm.instructions.len());

        for idx in start_idx..end_idx {
            // Check if this address is a function entry point (only if debug symbols are enabled)
            if self.show_debug_symbols {
                if let Some(func_name) = vm.debug_symbols.get(&idx) {
                    // Add a function label line
                    let mut label_spans = vec![];
                    label_spans.push(Span::raw("  "));
                    label_spans.push(Span::styled(
                        format!("{func_name}:"),
                        Style::default().fg(Color::Magenta).add_modifier(Modifier::BOLD)
                    ));
                    items.push(ListItem::new(Line::from(label_spans)));
                }
            }
            
            let instr = &vm.instructions[idx];
            let is_current = idx == current_idx;
            let breakpoint_state = self.breakpoints.get(&idx).copied();
            let in_history = self.execution_history.contains(&idx);

            // Format the instruction
            let addr = format!("{idx:04X}");
            
            // Build the line with appropriate styling
            let mut spans = vec![];

            // Breakpoint/current/history indicator (2 character width)
            // First character: breakpoint status
            // Second character: current position or space
            
            // First show breakpoint indicator if present
            match breakpoint_state {
                Some(true) => {
                    // Active breakpoint
                    spans.push(Span::styled("●", Style::default().fg(Color::Red)));
                }
                Some(false) => {
                    // Inactive/disabled breakpoint - use yellow for better visibility
                    spans.push(Span::styled("○", Style::default().fg(Color::Yellow)));
                }
                None => {
                    // No breakpoint
                    if in_history {
                        spans.push(Span::styled("·", Style::default().fg(Color::Gray)));
                    } else {
                        spans.push(Span::raw(" "));
                    }
                }
            }
            
            // Then show current position indicator
            if is_current {
                spans.push(Span::styled("→", Style::default().fg(Color::Rgb(0, 200, 0)).add_modifier(Modifier::BOLD)));
            } else {
                spans.push(Span::raw(" "));
            }

            // Address
            spans.push(Span::styled(
                addr,
                if is_current {
                    Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)
                } else {
                    Style::default().fg(Color::DarkGray)
                }
            ));

            spans.push(Span::raw("  "));

            // Add hex view if enabled
            if self.show_instruction_hex {
                // Show the 8 bytes of the instruction
                let bytes = [
                    instr.opcode,
                    instr.word0,
                    (instr.word1 & 0xFF) as u8,
                    ((instr.word1 >> 8) & 0xFF) as u8,
                    (instr.word2 & 0xFF) as u8,
                    ((instr.word2 >> 8) & 0xFF) as u8,
                    (instr.word3 & 0xFF) as u8,
                    ((instr.word3 >> 8) & 0xFF) as u8,
                ];
                
                for (byte_idx, &byte) in bytes.iter().enumerate() {
                    // Check if this byte is under cursor
                    let relative_row = idx.saturating_sub(self.disasm_scroll);
                    let is_cursor = self.focused_pane == crate::tui_debugger::FocusedPane::Disassembly
                        && relative_row == self.disasm_cursor_row
                        && byte_idx == self.disasm_cursor_byte;
                    
                    let style = if is_cursor {
                        Style::default().bg(Color::Yellow).fg(Color::Black)
                    } else if byte == 0 {
                        Style::default().fg(Color::DarkGray)
                    } else {
                        Style::default().fg(Color::Cyan)
                    };
                    
                    spans.push(Span::styled(format!("{byte:02X}"), style));
                    if byte_idx < 7 {
                        spans.push(Span::raw(" "));
                    }
                }
                
                spans.push(Span::raw("  "));
            }

            // Add instruction spans with syntax highlighting
            let instr_spans = self.format_instruction_spans(instr, vm);
            spans.extend(instr_spans);

            items.push(ListItem::new(Line::from(spans)));
        }

        let title = format!(" Disassembly [{}] ", if self.focused_pane == crate::tui_debugger::FocusedPane::Disassembly { "ACTIVE" } else { "F1" });
        let block = Block::default()
            .title(title)
            .borders(Borders::ALL)
            .border_style(if self.focused_pane == crate::tui_debugger::FocusedPane::Disassembly {
                Style::default().fg(Color::Cyan)
            } else {
                Style::default().fg(Color::Gray)
            });

        let list = List::new(items).block(block);
        frame.render_widget(list, area);
    }

    fn format_instruction_spans(&self, instr: &Instr, vm: &VM) -> Vec<Span<'static>> {
        let mut spans = vec![];
        
        // Get opcode name and style
        let opcode_str = Self::opcode_name(instr.opcode);
        let opcode_style = self.get_instruction_style(instr);
        
        // Convert u8 to Opcode enum for cleaner matching
        let opcode = Opcode::from_u8(instr.opcode);
        
        match opcode {
            Some(Opcode::Nop) => {
                if instr.word0 == 0 && instr.word1 == 0 && instr.word2 == 0 && instr.word3 == 0 {
                    spans.push(Span::styled("HALT", Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)));
                } else {
                    spans.push(Span::styled("NOP", opcode_style));
                }
            },
            Some(Opcode::Add) | Some(Opcode::Sub) | Some(Opcode::And) | Some(Opcode::Or) | 
            Some(Opcode::Xor) | Some(Opcode::Sll) | Some(Opcode::Srl) | Some(Opcode::Slt) | 
            Some(Opcode::Sltu) | Some(Opcode::Mul) | Some(Opcode::Div) | Some(Opcode::Mod) |
            Some(Opcode::Loadc) | Some(Opcode::Storc) => {
                // R-type: op rd, rs, rt
                spans.push(Span::styled(format!("{opcode_str:<6} "), opcode_style));
                spans.push(Span::styled(Self::register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0)))); // Dark green
                spans.push(Span::raw(", "));
                spans.push(Span::styled(Self::register_name(instr.word2 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
                spans.push(Span::raw(", "));
                spans.push(Span::styled(Self::register_name(instr.word3 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
            },
            Some(Opcode::Addi) | Some(Opcode::Andi) | Some(Opcode::Ori) | Some(Opcode::Xori) | 
            Some(Opcode::Slli) | Some(Opcode::Srli) | Some(Opcode::Muli) | Some(Opcode::Divi) | Some(Opcode::Modi) => {
                // I-type with immediate: op rd, rs, imm
                spans.push(Span::styled(format!("{opcode_str:<6} "), opcode_style));
                spans.push(Span::styled(Self::register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
                spans.push(Span::raw(", "));
                spans.push(Span::styled(Self::register_name(instr.word2 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
                spans.push(Span::raw(", "));
                spans.push(Span::styled(format!("0x{:X}", instr.word3), Style::default().fg(Color::Rgb(255, 140, 0)))); // Orange
            },
            Some(Opcode::Li) => {
                // LI: li rd, imm
                spans.push(Span::styled(format!("{opcode_str:<6} "), opcode_style));
                spans.push(Span::styled(Self::register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
                spans.push(Span::raw(", "));
                spans.push(Span::styled(format!("0x{:X}", instr.word2), Style::default().fg(Color::Rgb(255, 140, 0)))); // Orange
            },
            Some(Opcode::Load) | Some(Opcode::Store) => {
                // LOAD/STORE: op r, bank, addr
                spans.push(Span::styled(format!("{opcode_str:<6} "), opcode_style));
                spans.push(Span::styled(Self::register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
                spans.push(Span::raw(", "));
                
                // Bank operand (could be register or immediate)
                if instr.word2 < 32 {
                    spans.push(Span::styled(Self::register_name(instr.word2 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
                } else {
                    spans.push(Span::styled(format!("0x{:X}", instr.word2), Style::default().fg(Color::Rgb(255, 140, 0))));
                }
                spans.push(Span::raw(", "));
                
                // Address operand (could be register or immediate)
                if instr.word3 < 32 {
                    spans.push(Span::styled(Self::register_name(instr.word3 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
                } else {
                    spans.push(Span::styled(format!("0x{:X}", instr.word3), Style::default().fg(Color::Rgb(255, 140, 0))));
                }
            },
            Some(Opcode::Jal) => {
                // JAL: jal rd, addr
                spans.push(Span::styled(format!("{opcode_str:<6} "), opcode_style));
                spans.push(Span::styled(Self::register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
                spans.push(Span::raw(", "));
                
                // Check if we have a function name for this address (only if debug symbols are enabled)
                let target_addr = instr.word3 as usize;
                if self.show_debug_symbols {
                    if let Some(func_name) = vm.debug_symbols.get(&target_addr) {
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
                spans.push(Span::styled(Self::register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
                spans.push(Span::raw(", "));
                spans.push(Span::styled(Self::register_name(instr.word3 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
            },
            Some(Opcode::Beq) | Some(Opcode::Bne) | Some(Opcode::Blt) | Some(Opcode::Bge) => {
                // Branch: beq/bne/blt/bge rs, rt, offset
                spans.push(Span::styled(format!("{opcode_str:<6} "), opcode_style));
                spans.push(Span::styled(Self::register_name(instr.word1 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
                spans.push(Span::raw(", "));
                spans.push(Span::styled(Self::register_name(instr.word2 as u8), Style::default().fg(Color::Rgb(0, 200, 0))));
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
    
    pub(crate) fn format_instruction(&self, instr: &Instr) -> String {
        let opcode_str = Self::opcode_name(instr.opcode);
        let opcode = Opcode::from_u8(instr.opcode);

        match opcode {
            Some(Opcode::Nop) => {
                if instr.word0 == 0 && instr.word1 == 0 && instr.word2 == 0 && instr.word3 == 0 {
                    "HALT".to_string()
                } else {
                    "NOP".to_string()
                }
            },
            Some(Opcode::Add) | Some(Opcode::Sub) | Some(Opcode::And) | Some(Opcode::Or) | 
            Some(Opcode::Xor) | Some(Opcode::Sll) | Some(Opcode::Srl) | Some(Opcode::Slt) | 
            Some(Opcode::Sltu) | Some(Opcode::Mul) | Some(Opcode::Div) | Some(Opcode::Mod) |
            Some(Opcode::Loadc) | Some(Opcode::Storc) => {
                let rd = Self::register_name(instr.word1 as u8);
                let rs = Self::register_name(instr.word2 as u8);
                let rt = Self::register_name(instr.word3 as u8);
                format!("{opcode_str:<6} {rd}, {rs}, {rt}")
            },
            Some(Opcode::Addi) | Some(Opcode::Andi) | Some(Opcode::Ori) | Some(Opcode::Xori) | 
            Some(Opcode::Slli) | Some(Opcode::Srli) | Some(Opcode::Muli) | Some(Opcode::Divi) | Some(Opcode::Modi) => {
                let rd = Self::register_name(instr.word1 as u8);
                let rs = Self::register_name(instr.word2 as u8);
                format!("{:<6} {}, {}, 0x{:X}", opcode_str, rd, rs, instr.word3)
            },
            Some(Opcode::Li) => {
                let rd = Self::register_name(instr.word1 as u8);
                format!("{:<6} {}, 0x{:X}", opcode_str, rd, instr.word2)
            },
            Some(Opcode::Load) | Some(Opcode::Store) => {
                let r = Self::register_name(instr.word1 as u8);
                let bank = Self::format_operand(instr.word2);
                let addr = Self::format_operand(instr.word3);
                format!("{opcode_str:<6} {r}, {bank}, {addr}")
            },
            Some(Opcode::Jal) => {
                let rd = Self::register_name(instr.word1 as u8);
                format!("{:<6} {}, 0x{:04X}", opcode_str, rd, instr.word3)
            },
            Some(Opcode::Jalr) => {
                let rd = Self::register_name(instr.word1 as u8);
                let rs = Self::register_name(instr.word3 as u8);
                format!("{opcode_str:<6} {rd}, {rs}")
            },
            Some(Opcode::Beq) | Some(Opcode::Bne) | Some(Opcode::Blt) | Some(Opcode::Bge) => {
                let rs = Self::register_name(instr.word1 as u8);
                let rt = Self::register_name(instr.word2 as u8);
                let offset = instr.word3 as i16;
                format!("{opcode_str:<6} {rs}, {rt}, {offset}")
            },
            Some(Opcode::Brk) => "BRK".to_string(),
            None => format!("??? 0x{:02X}", instr.opcode),
        }
    }

    fn get_instruction_style(&self, instr: &Instr) -> Style {
        let opcode = Opcode::from_u8(instr.opcode);
        
        match opcode {
            Some(Opcode::Nop) if instr.word1 == 0 && instr.word2 == 0 && instr.word3 == 0 => {
                // HALT instruction (all zeros)
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

    fn opcode_name(opcode: u8) -> &'static str {
        // Use the from_u8 and to_str methods from Opcode
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

    fn format_operand(value: u16) -> String {
        // Check if this is a valid register (0-31)
        if value < 32 {
            Self::register_name(value as u8).to_string()
        } else {
            format!("0x{value:X}")
        }
    }
}