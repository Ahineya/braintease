use crate::tui_debugger::{FocusedPane, TuiDebugger};
use crate::vm::VM;
use ratatui::layout::Rect;
use ratatui::prelude::{Color, Line, Span, Style};
use ratatui::widgets::{Block, Borders, Paragraph, Wrap};
use ratatui::Frame;

impl TuiDebugger {
    pub(crate) fn draw_output(&self, frame: &mut Frame, area: Rect, vm: &VM) {
        // Get output from VM's buffer
        let output_bytes: Vec<u8> = vm.output_buffer.iter().cloned().collect();
        let output_text = String::from_utf8_lossy(&output_bytes);
        let lines: Vec<Line> = output_text
            .lines()
            .skip(self.output_scroll)
            .map(|line| Line::from(Span::raw(line)))
            .collect();

        let title = format!(
            " Output [{}] ",
            if self.focused_pane == FocusedPane::Output {
                "ACTIVE"
            } else {
                "F7"
            }
        );
        let block = Block::default()
            .title(title)
            .borders(Borders::ALL)
            .border_style(if self.focused_pane == FocusedPane::Output {
                Style::default().fg(Color::Cyan)
            } else {
                Style::default().fg(Color::Gray)
            });

        let paragraph = Paragraph::new(lines)
            .block(block)
            .wrap(Wrap { trim: false });
        frame.render_widget(paragraph, area);
    }
}
