use crate::tui_debugger::{FocusedPane, TuiDebugger};
use crate::vm::{VMState, VM};
use ratatui::layout::Rect;
use ratatui::prelude::{Color, Line, Modifier, Span, Style};
use ratatui::widgets::Paragraph;
use ratatui::Frame;

impl TuiDebugger {
    pub(crate) fn draw_status_line(&self, frame: &mut Frame, area: Rect, vm: &VM) {
        let mut spans = vec![];

        // Show VM state
        let state_color = match vm.state {
            VMState::Running => Color::Green,
            VMState::Halted => Color::Red,
            VMState::Breakpoint => Color::Yellow,
            VMState::Error(_) => Color::Red,
            VMState::Setup => Color::Gray,
        };

        spans.push(Span::styled(
            format!(
                " {} ",
                match vm.state {
                    VMState::Running => "RUNNING",
                    VMState::Halted => "HALTED",
                    VMState::Breakpoint => "BREAKPOINT",
                    VMState::Error(_) => "ERROR",
                    VMState::Setup => "SETUP",
                }
            ),
            Style::default()
                .bg(state_color)
                .fg(Color::Black)
                .add_modifier(Modifier::BOLD),
        ));

        // Show status message if present
        if let Some(ref msg) = self.status_message {
            spans.push(Span::raw(" "));
            spans.push(Span::styled(
                format!(" {msg} "),
                Style::default()
                    .bg(Color::Yellow)
                    .fg(Color::Black)
                    .add_modifier(Modifier::BOLD),
            ));
        }

        spans.push(Span::raw(" "));

        // Show active pane
        spans.push(Span::styled(
            format!(
                "Active: {}",
                match self.focused_pane {
                    FocusedPane::Disassembly => "Disassembly",
                    FocusedPane::Registers => "Registers",
                    FocusedPane::Memory => "Memory",
                    FocusedPane::Stack => "Stack",
                    FocusedPane::CallStack => "Call Stack",
                    FocusedPane::Watches => "Watches",
                    FocusedPane::Breakpoints => "Breakpoints",
                    FocusedPane::Output => "Output",
                    FocusedPane::Command => "Command",
                }
            ),
            Style::default().fg(Color::Cyan),
        ));

        // Show hints based on context
        let hints = match self.focused_pane {
            FocusedPane::Disassembly => " | Space:step b:breakpoint r:run",
            FocusedPane::Memory => " | g:goto e:edit",
            FocusedPane::Watches => " | w:add W:remove",
            FocusedPane::Breakpoints => " | Space:toggle d:delete",
            FocusedPane::CallStack => " | Enter:goto ↑↓:navigate",
            _ => " | ?:help q:quit",
        };
        spans.push(Span::styled(hints, Style::default().fg(Color::DarkGray)));

        // Show hidden panels indicator
        let mut hidden_panels = vec![];
        if !self.show_registers {
            hidden_panels.push("Reg");
        }
        if !self.show_memory {
            hidden_panels.push("Mem");
        }
        if !self.show_stack {
            hidden_panels.push("Stk");
        }
        if !self.show_callstack {
            hidden_panels.push("CS");
        }
        if !self.show_watches {
            hidden_panels.push("Wat");
        }
        if !self.show_breakpoints {
            hidden_panels.push("BP");
        }
        if !self.show_output {
            hidden_panels.push("Out");
        }

        if !hidden_panels.is_empty() {
            spans.push(Span::raw(" "));
            spans.push(Span::styled(
                format!(" Hidden: {} ", hidden_panels.join(",")),
                Style::default()
                    .fg(Color::Magenta)
                    .add_modifier(Modifier::ITALIC),
            ));
        }

        // Right-align some info
        let total_breakpoints = self.breakpoints.len();
        if total_breakpoints > 0 {
            let active_breakpoints = self
                .breakpoints
                .values()
                .filter(|&&enabled| enabled)
                .count();
            let bp_text = format!(" {active_breakpoints}/{total_breakpoints} BP ");
            let used_width = spans.iter().map(|s| s.content.len()).sum::<usize>();
            let padding = (area.width as usize).saturating_sub(used_width + bp_text.len());
            if padding > 0 {
                spans.push(Span::raw(" ".repeat(padding)));
            }
            let bp_color = if active_breakpoints > 0 {
                Color::Red
            } else {
                Color::Yellow
            };
            spans.push(Span::styled(bp_text, Style::default().fg(bp_color)));
        }

        let paragraph = Paragraph::new(Line::from(spans));
        frame.render_widget(paragraph, area);
    }
}
