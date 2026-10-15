use crate::tui_debugger::TuiDebugger;
use ratatui::layout::{Alignment, Rect};
use ratatui::prelude::{Color, Line, Modifier, Span, Style};
use ratatui::widgets::{Block, Borders, Clear, Paragraph};
use ratatui::Frame;

impl TuiDebugger {
    pub(crate) fn draw_help(&mut self, frame: &mut Frame, area: Rect) {
        // Compact help text - fits better in window
        let all_help = vec![
            Line::from(""), // Empty line at top for spacing
            Line::from(Span::styled(
                "── Navigation ──",
                Style::default().fg(Color::Yellow),
            )),
            Line::from("F1-F7,Tab  Panes | hjkl/↑↓←→  Move"),
            Line::from(""),
            Line::from(Span::styled(
                "── Mouse Support ──",
                Style::default().fg(Color::Yellow),
            )),
            Line::from("Click: Select panel"),
            Line::from("Scroll: Navigate in panel"),
            Line::from("Dbl-click Disasm: Toggle breakpoint"),
            Line::from("Dbl-click Memory: Edit cell"),
            Line::from(""),
            Line::from(Span::styled(
                "── Execution ──",
                Style::default().fg(Color::Yellow),
            )),
            Line::from("Space/s  Step | r  Run | c  Continue"),
            Line::from("R  Restart | b  Breakpoint"),
            Line::from(""),
            Line::from(Span::styled(
                "── Disassembly ──",
                Style::default().fg(Color::Yellow),
            )),
            Line::from("Shift+H  Hex view | 0-9,a-f  Edit"),
            Line::from("Shift+D  Toggle debug symbols"),
            Line::from(""),
            Line::from(Span::styled(
                "── Memory ──",
                Style::default().fg(Color::Yellow),
            )),
            Line::from("g  Go addr | Shift+G  Stack"),
            Line::from("[  Prev bank | ]  Next bank"),
            Line::from("a  ASCII | e  Edit | w/W  Watch"),
            Line::from(""),
            Line::from(Span::styled(
                "── Panels (T+#) ──",
                Style::default().fg(Color::Yellow),
            )),
            Line::from("Shift+T then: 2-7 to toggle"),
            Line::from("2 Reg | 3 Mem | 4 Stack"),
            Line::from("5 Watch | 6 Break | 7 Output"),
            Line::from(""),
            Line::from(Span::styled(
                "── Commands (:) ──",
                Style::default().fg(Color::Yellow),
            )),
            Line::from(":break <a> | :mem <a> <v>"),
            Line::from(":bank <n> | :reg <#> <v>"),
            Line::from(":q  Quit"),
            Line::from(""),
            Line::from(Span::styled(
                "── Edit Formats ──",
                Style::default().fg(Color::Yellow),
            )),
            Line::from("addr:0xFF  Hex | addr:255  Dec"),
            Line::from("addr:'A'  Char | addr:\"hi\"  Str"),
        ];

        // Calculate visible area
        let help_width = 40;
        let help_height = area.height.min(25);
        let help_area = Rect::new(
            (area.width.saturating_sub(help_width)) / 2,
            (area.height.saturating_sub(help_height)) / 2,
            help_width,
            help_height,
        );

        // Calculate scrolling
        let visible_lines = help_height.saturating_sub(2) as usize; // Account for borders
        let total_lines = all_help.len();

        // Account for header lines when calculating max scroll
        let header_lines = 4; // Header, scroll info, close instruction, separator
        let content_lines = visible_lines.saturating_sub(header_lines);
        let max_scroll = total_lines.saturating_sub(content_lines);

        // Ensure scroll is within bounds
        if self.help_scroll > max_scroll {
            self.help_scroll = max_scroll;
        }

        // Build the display content with header and footer
        let mut display_lines = Vec::new();

        // Always show the header at the top
        display_lines.push(Line::from(Span::styled(
            "RVM Debugger Help",
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        )));

        // Show scroll indicator if content is scrollable
        if total_lines > visible_lines {
            display_lines.push(Line::from(vec![
                Span::styled("[↑/↓ scroll] ", Style::default().fg(Color::Yellow)),
                Span::styled(
                    format!(
                        "Lines {}-{}/{}",
                        self.help_scroll + 1,
                        (self.help_scroll + visible_lines).min(total_lines),
                        total_lines
                    ),
                    Style::default().fg(Color::DarkGray),
                ),
            ]));
        } else {
            display_lines.push(Line::from(""));
        }

        display_lines.push(Line::from(Span::styled(
            "Press ? or ESC to close",
            Style::default().fg(Color::Green),
        )));
        display_lines.push(Line::from("")); // Separator line

        // Calculate how many content lines we can show (accounting for header lines)
        let header_lines = 4; // Header, scroll info, close instruction, separator
        let content_lines = visible_lines.saturating_sub(header_lines);

        // Get the visible portion of help content
        let end = (self.help_scroll + content_lines).min(total_lines);
        let visible_help: Vec<Line> = all_help[self.help_scroll..end].to_vec();

        // Add the scrollable content
        display_lines.extend(visible_help);

        // Simple title for the window border
        let title = " Help ";

        let block = Block::default()
            .title(title)
            .borders(Borders::ALL)
            .border_style(Style::default().fg(Color::Cyan));

        let paragraph = Paragraph::new(display_lines)
            .block(block)
            .alignment(Alignment::Left);

        frame.render_widget(Clear, help_area);
        frame.render_widget(paragraph, help_area);
    }
}
