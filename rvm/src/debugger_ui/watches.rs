use crate::tui_debugger::{FocusedPane, TuiDebugger};
use crate::vm::VM;
use ratatui::layout::Rect;
use ratatui::prelude::{Color, Line, Span, Style};
use ratatui::widgets::{Block, Borders, Paragraph};
use ratatui::Frame;

impl TuiDebugger {
    pub(crate) fn draw_watches(&mut self, frame: &mut Frame, area: Rect, vm: &VM) {
        let mut text = Vec::new();

        if self.memory_watches.is_empty() {
            text.push(Line::from(Span::styled(
                "No watches set",
                Style::default().fg(Color::DarkGray),
            )));
            text.push(Line::from(""));
            text.push(Line::from(Span::raw("Press 'w' to add a watch")));
        } else {
            // Calculate visible area
            let visible_lines = area.height.saturating_sub(2) as usize; // Account for borders
            let total_watches = self.memory_watches.len();

            // Auto-scroll to keep selected item visible
            if self.selected_watch < self.watches_scroll {
                self.watches_scroll = self.selected_watch;
            } else if self.selected_watch >= self.watches_scroll + visible_lines {
                self.watches_scroll = self.selected_watch.saturating_sub(visible_lines - 1);
            }

            // Limit scroll to valid range
            self.watches_scroll = self
                .watches_scroll
                .min(total_watches.saturating_sub(visible_lines));

            // Show only visible watches
            let start = self.watches_scroll;
            let end = (start + visible_lines).min(total_watches);

            for i in start..end {
                let watch = &self.memory_watches[i];
                let is_selected =
                    i == self.selected_watch && self.focused_pane == FocusedPane::Watches;

                let mut spans = vec![];
                if is_selected {
                    spans.push(Span::styled("→ ", Style::default().fg(Color::Yellow)));
                } else {
                    spans.push(Span::raw("  "));
                }

                spans.push(Span::styled(&watch.name, Style::default().fg(Color::Cyan)));
                spans.push(Span::raw(": "));

                // Get value from memory
                if watch.address < vm.memory.len() {
                    let value = vm.memory[watch.address];
                    let formatted = match watch.format {
                        crate::tui_debugger::WatchFormat::Hex => format!("0x{value:04X}"),
                        crate::tui_debugger::WatchFormat::Decimal => format!("{value}"),
                        crate::tui_debugger::WatchFormat::Char => {
                            let ch = (value & 0xFF) as u8;
                            if (0x20..0x7F).contains(&ch) {
                                format!("'{}'", ch as char)
                            } else {
                                format!("\\x{ch:02X}")
                            }
                        }
                        crate::tui_debugger::WatchFormat::Binary => format!("{value:016b}"),
                    };
                    spans.push(Span::styled(formatted, Style::default().fg(Color::White)));
                } else {
                    spans.push(Span::styled("Invalid", Style::default().fg(Color::Red)));
                }

                text.push(Line::from(spans));
            }
        }

        let scroll_indicator = if !self.memory_watches.is_empty() {
            format!(
                " [{}/{}]",
                self.selected_watch + 1,
                self.memory_watches.len()
            )
        } else {
            String::new()
        };

        let title = format!(
            " Watches{} [{}] ",
            scroll_indicator,
            if self.focused_pane == FocusedPane::Watches {
                "ACTIVE"
            } else {
                "F5"
            }
        );
        let block = Block::default()
            .title(title)
            .borders(Borders::ALL)
            .border_style(if self.focused_pane == FocusedPane::Watches {
                Style::default().fg(Color::Cyan)
            } else {
                Style::default().fg(Color::Gray)
            });

        let paragraph = Paragraph::new(text).block(block);
        frame.render_widget(paragraph, area);
    }
}
