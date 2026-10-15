use crate::tui_debugger::TuiDebugger;
use ratatui::layout::Rect;
use ratatui::prelude::{Color, Line, Modifier, Span, Style};
use ratatui::widgets::{Clear, Paragraph};
use ratatui::Frame;

impl TuiDebugger {
    pub(crate) fn draw_input_line(&self, frame: &mut Frame, area: Rect, prompt: &str) {
        let input_area = area; // Use the provided area directly

        // Clear the line first
        frame.render_widget(Clear, input_area);

        // Create the input line with prompt
        let mut spans = vec![];

        // Add prompt with appropriate styling
        let prompt_text = format!("[{prompt}] ");
        spans.push(Span::styled(
            prompt_text,
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        ));

        // Show what the user is typing
        if self.mode == crate::tui_debugger::DebuggerMode::Command {
            spans.push(Span::styled(":", Style::default().fg(Color::Yellow)));
        }
        spans.push(Span::styled(
            &self.command_buffer,
            Style::default().fg(Color::White),
        ));

        // Add blinking cursor
        spans.push(Span::styled(
            "█",
            Style::default()
                .fg(Color::White)
                .add_modifier(Modifier::SLOW_BLINK),
        ));

        // Add hint on the right side
        let hint = " (ESC to cancel)";
        let used_width = prompt.len() + 3 + self.command_buffer.len() + 1 + hint.len();
        let padding_len = (area.width as usize).saturating_sub(used_width);
        if padding_len > 0 {
            spans.push(Span::raw(" ".repeat(padding_len)));
            spans.push(Span::styled(hint, Style::default().fg(Color::DarkGray)));
        }

        let paragraph = Paragraph::new(Line::from(spans));
        frame.render_widget(paragraph, input_area);
    }
}
