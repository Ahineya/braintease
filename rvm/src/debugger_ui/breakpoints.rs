use crate::tui_debugger::{FocusedPane, TuiDebugger};
use crate::vm::VM;
use ratatui::layout::Rect;
use ratatui::prelude::{Color, Line, Modifier, Span, Style};
use ratatui::widgets::{Block, Borders, Paragraph};
use ratatui::Frame;

impl TuiDebugger {
    pub(crate) fn draw_breakpoints(&mut self, frame: &mut Frame, area: Rect, vm: &VM) {
        let mut text = Vec::new();

        if self.breakpoints.is_empty() {
            text.push(Line::from(Span::styled(
                "No breakpoints set",
                Style::default().fg(Color::DarkGray),
            )));
            text.push(Line::from(""));
            text.push(Line::from(Span::raw("Press 'b' at cursor")));
            text.push(Line::from(Span::raw("or Shift+B for address")));
            text.push(Line::from(""));
            text.push(Line::from(Span::raw("When focused:")));
            text.push(Line::from(Span::raw("  Space - Toggle enable/disable")));
            text.push(Line::from(Span::raw("  d/Del - Delete breakpoint")));
        } else {
            // Sort breakpoints for consistent display
            let mut sorted_breakpoints: Vec<_> = self.breakpoints.iter().collect();
            sorted_breakpoints.sort_by_key(|(&addr, _)| addr);

            // Calculate visible area
            let visible_lines = area.height.saturating_sub(2) as usize;
            let total_breakpoints = sorted_breakpoints.len();

            // Auto-scroll to keep selected item visible
            if self.selected_breakpoint < self.breakpoints_scroll {
                self.breakpoints_scroll = self.selected_breakpoint;
            } else if self.selected_breakpoint >= self.breakpoints_scroll + visible_lines {
                self.breakpoints_scroll =
                    self.selected_breakpoint.saturating_sub(visible_lines - 1);
            }

            // Limit scroll to valid range
            self.breakpoints_scroll = self
                .breakpoints_scroll
                .min(total_breakpoints.saturating_sub(visible_lines));

            // Show only visible breakpoints
            let start = self.breakpoints_scroll;
            let end = (start + visible_lines).min(total_breakpoints);

            for i in start..end {
                let (&addr, &enabled) = sorted_breakpoints[i];
                let is_selected =
                    i == self.selected_breakpoint && self.focused_pane == FocusedPane::Breakpoints;

                let mut spans = vec![];

                // Selection indicator
                if is_selected {
                    spans.push(Span::styled("→ ", Style::default().fg(Color::Yellow)));
                } else {
                    spans.push(Span::raw("  "));
                }

                // Enabled/disabled indicator
                if enabled {
                    spans.push(Span::styled("● ", Style::default().fg(Color::Red)));
                } else {
                    spans.push(Span::styled("○ ", Style::default().fg(Color::Yellow)));
                }

                // Mark current PC breakpoint
                let pc = vm.registers[ripple_asm::Register::Pc as usize] as usize;
                let pcb = vm.registers[ripple_asm::Register::Pcb as usize] as usize;
                let current_addr = pcb * vm.bank_size as usize + pc;

                if addr == current_addr {
                    spans.push(Span::styled(
                        "► ",
                        Style::default()
                            .fg(Color::Green)
                            .add_modifier(Modifier::BOLD),
                    ));
                } else {
                    spans.push(Span::raw("  "));
                }

                // Show breakpoint address
                let addr_style = if enabled {
                    Style::default().fg(Color::Red)
                } else {
                    Style::default()
                        .fg(Color::Yellow)
                        .add_modifier(Modifier::CROSSED_OUT)
                };
                spans.push(Span::styled(format!("{addr:04X}"), addr_style));

                // Show instruction at that address if available
                if addr < vm.instructions.len() {
                    let instr = &vm.instructions[addr];
                    let instr_str = self.format_instruction(instr);
                    spans.push(Span::raw(": "));

                    let instr_style = if enabled {
                        Style::default().fg(Color::White)
                    } else {
                        Style::default().fg(Color::DarkGray)
                    };
                    spans.push(Span::styled(instr_str, instr_style));
                }

                text.push(Line::from(spans));
            }
        }

        let active_count = self
            .breakpoints
            .values()
            .filter(|&&enabled| enabled)
            .count();
        let total_count = self.breakpoints.len();

        // Show active/total and position only if there are breakpoints
        let status = if total_count > 0 {
            format!(
                " {}/{} active, #{}/{}",
                active_count,
                total_count,
                self.selected_breakpoint + 1,
                total_count
            )
        } else {
            String::new()
        };

        let title = format!(
            " Breakpoints{} [{}] ",
            status,
            if self.focused_pane == FocusedPane::Breakpoints {
                "ACTIVE"
            } else {
                "F6"
            }
        );
        let block = Block::default()
            .title(title)
            .borders(Borders::ALL)
            .border_style(if self.focused_pane == FocusedPane::Breakpoints {
                Style::default().fg(Color::Cyan)
            } else {
                Style::default().fg(Color::Gray)
            });

        let paragraph = Paragraph::new(text).block(block);
        frame.render_widget(paragraph, area);
    }
}
