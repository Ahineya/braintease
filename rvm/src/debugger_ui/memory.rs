use crate::tui_debugger::{FocusedPane, TuiDebugger};
use crate::vm::VM;
use ratatui::layout::Rect;
use ratatui::prelude::{Color, Line, Modifier, Span, Style};
use ratatui::widgets::{Block, Borders, Paragraph};
use ratatui::Frame;
use ripple_asm::Register;

impl TuiDebugger {
    pub(crate) fn draw_memory(&self, frame: &mut Frame, area: Rect, vm: &VM) {
        let mut text = Vec::new();

        // Fixed 8 columns for cognitive consistency
        let bytes_per_row = 8;

        let visible_rows = area.height.saturating_sub(3) as usize;

        let start_addr = self.memory_base_addr;

        // Calculate the cursor's absolute address
        let cursor_addr =
            self.memory_base_addr + self.memory_scroll * bytes_per_row + self.memory_cursor_col;

        // Get stack information
        let sp = vm.registers[Register::Sp as usize];
        let sb = vm.registers[Register::Sb as usize];
        let fp = vm.registers[Register::Fp as usize];
        let stack_base_addr = sb as usize * vm.bank_size as usize;
        let stack_top_addr = stack_base_addr + sp as usize;
        let frame_addr = stack_base_addr + fp as usize;

        for row in 0..visible_rows {
            let addr = start_addr + row * bytes_per_row;
            if addr >= vm.memory.len() {
                break;
            }

            // Check if this row contains stack-related addresses
            let mut row_indicator = "    ";
            let mut indicator_color = Color::DarkGray;

            // Check if any address in this row is stack-related
            for col in 0..bytes_per_row {
                let check_addr = addr + col;
                if check_addr == stack_top_addr {
                    row_indicator = "SP→ ";
                    indicator_color = Color::Green;
                    break;
                } else if check_addr == frame_addr {
                    row_indicator = "FP→ ";
                    indicator_color = Color::Cyan;
                    break;
                } else if check_addr >= stack_base_addr && check_addr < stack_top_addr {
                    row_indicator = "STK ";
                    indicator_color = Color::Yellow;
                    // Don't break - SP or FP might be in same row
                }
            }

            let mut spans = vec![
                Span::styled(row_indicator, Style::default().fg(indicator_color)),
                Span::styled(
                    format!("{addr:04X}: "),
                    Style::default().fg(Color::DarkGray),
                ),
            ];

            // Hex values
            for col in 0..bytes_per_row {
                let idx = addr + col;
                if idx < vm.memory.len() {
                    let value = vm.memory[idx];

                    // Check if this is the cursor position
                    let is_cursor = idx == cursor_addr && self.focused_pane == FocusedPane::Memory;

                    // Check if this is a stack-related address
                    let is_stack = idx >= stack_base_addr && idx < stack_top_addr;
                    let is_sp = idx == stack_top_addr;
                    let is_fp = idx == frame_addr;

                    let style = if is_cursor {
                        // Highlight cursor position
                        Style::default().bg(Color::Yellow).fg(Color::Black)
                    } else if is_sp {
                        // Stack pointer position
                        Style::default()
                            .fg(Color::Green)
                            .add_modifier(Modifier::BOLD)
                    } else if is_fp {
                        // Frame pointer position
                        Style::default()
                            .fg(Color::Cyan)
                            .add_modifier(Modifier::BOLD)
                    } else if is_stack {
                        // Stack area
                        if value != 0 {
                            Style::default().fg(Color::Yellow)
                        } else {
                            Style::default()
                                .fg(Color::Yellow)
                                .add_modifier(Modifier::DIM)
                        }
                    } else if idx < 2 {
                        // Special I/O registers
                        Style::default().fg(Color::Magenta)
                    } else if value != 0 {
                        Style::default().fg(Color::White)
                    } else {
                        Style::default().fg(Color::DarkGray)
                    };

                    // Apply style only to the hex value, not the space
                    spans.push(Span::styled(format!("{value:04X}"), style));
                    spans.push(Span::raw(" ")); // Add space separately
                } else {
                    spans.push(Span::raw("     "));
                }
            }

            // ASCII representation if enabled
            if self.show_ascii {
                spans.push(Span::raw(" | "));

                for col in 0..bytes_per_row {
                    let idx = addr + col;
                    if idx < vm.memory.len() {
                        let value = (vm.memory[idx] & 0xFF) as u8;
                        let ch = if (0x20..0x7F).contains(&value) {
                            value as char
                        } else {
                            '.'
                        };

                        // Check if this is the cursor position
                        let is_cursor =
                            idx == cursor_addr && self.focused_pane == FocusedPane::Memory;

                        let style = if is_cursor {
                            Style::default().bg(Color::Yellow).fg(Color::Black)
                        } else if idx < 2 {
                            Style::default().fg(Color::Magenta)
                        } else if value != 0 {
                            Style::default().fg(Color::Cyan)
                        } else {
                            Style::default().fg(Color::DarkGray)
                        };
                        spans.push(Span::styled(ch.to_string(), style));
                    } else {
                        spans.push(Span::raw(" "));
                    }
                }
            }

            text.push(Line::from(spans));
        }

        // Calculate bank based on cursor position
        let cursor_bank = cursor_addr / vm.bank_size as usize;
        let total_banks = vm.memory.len().div_ceil(vm.bank_size as usize);

        let ascii_indicator = if self.show_ascii { " [ASCII]" } else { "" };
        let title = format!(
            " Memory Bank {}/{} @ {:04X}{} (cursor: {:04X}) [{}] ",
            cursor_bank,
            total_banks,
            self.memory_base_addr,
            ascii_indicator,
            cursor_addr,
            if self.focused_pane == FocusedPane::Memory {
                "ACTIVE"
            } else {
                "F3"
            }
        );
        let block = Block::default()
            .title(title)
            .borders(Borders::ALL)
            .border_style(if self.focused_pane == FocusedPane::Memory {
                Style::default().fg(Color::Cyan)
            } else {
                Style::default().fg(Color::Gray)
            });

        let paragraph = Paragraph::new(text).block(block);
        frame.render_widget(paragraph, area);
    }
}
