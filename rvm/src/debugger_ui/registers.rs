use crate::tui_debugger::{FocusedPane, TuiDebugger};
use crate::vm::VM;
use ratatui::layout::Rect;
use ratatui::prelude::{Color, Line, Modifier, Span, Style};
use ratatui::widgets::{Block, Borders, Paragraph};
use ratatui::Frame;
use ripple_asm::Register;

impl TuiDebugger {
    pub(crate) fn draw_registers(&self, frame: &mut Frame, area: Rect, vm: &VM) {
        let mut text = Vec::new();

        // First row: Hardware registers - PC and RA with banks, plus R0
        let mut spans = Vec::new();
        spans.push(Span::styled("PC:", Style::default().fg(Color::LightCyan)));
        spans.push(Span::styled(
            format!(
                "{:04X}:{:04X}",
                vm.registers[Register::Pcb as usize],
                vm.registers[Register::Pc as usize]
            ),
            Style::default()
                .fg(Color::Green)
                .add_modifier(Modifier::BOLD),
        ));
        spans.push(Span::raw("  "));
        spans.push(Span::styled("RA:", Style::default().fg(Color::LightCyan)));
        spans.push(Span::styled(
            format!(
                "{:04X}:{:04X}",
                vm.registers[Register::Rab as usize],
                vm.registers[Register::Ra as usize]
            ),
            Style::default()
                .fg(Color::Green)
                .add_modifier(Modifier::BOLD),
        ));
        spans.push(Span::raw("  "));
        spans.push(Span::styled("R0:", Style::default().fg(Color::LightCyan)));
        spans.push(Span::styled("0000", Style::default().fg(Color::DarkGray)));
        text.push(Line::from(spans));

        // Empty line after hardware registers
        text.push(Line::from(""));

        // Second row: Return values (RV0-RV1)
        let mut spans = Vec::new();
        for i in 5..=6 {
            let reg_name = match i {
                5 => "RV0",
                6 => "RV1",
                _ => unreachable!(),
            };
            spans.extend(self.format_register_with_color(reg_name, i, vm, Color::LightMagenta));
            spans.push(Span::raw("  "));
        }
        text.push(Line::from(spans));

        // Third row: Arguments (A0-A3)
        let mut spans = Vec::new();
        for i in 7..=10 {
            let reg_name = match i {
                7 => "A0",
                8 => "A1",
                9 => "A2",
                10 => "A3",
                _ => unreachable!(),
            };
            spans.extend(self.format_register_with_color(reg_name, i, vm, Color::LightBlue));
            spans.push(Span::raw("  "));
        }
        text.push(Line::from(spans));

        // Fourth row: Reserved/Extended (X0-X3)
        let mut spans = Vec::new();
        for i in 11..=14 {
            let reg_name = match i {
                11 => "X0",
                12 => "X1",
                13 => "X2",
                14 => "X3",
                _ => unreachable!(),
            };
            spans.extend(self.format_register_with_color(reg_name, i, vm, Color::Gray));
            spans.push(Span::raw("  "));
        }
        text.push(Line::from(spans));

        // Fifth row: Saved (S0-S3)
        let mut spans = Vec::new();
        for i in 23..=26 {
            let reg_name = format!("S{}", i - 23);
            spans.extend(self.format_register_with_color(&reg_name, i, vm, Color::LightGreen));
            spans.push(Span::raw("  "));
        }
        text.push(Line::from(spans));

        // Sixth row: Temporaries T0-T3
        let mut spans = Vec::new();
        for i in 15..=18 {
            let reg_name = format!("T{}", i - 15);
            spans.extend(self.format_register_with_color(&reg_name, i, vm, Color::LightYellow));
            spans.push(Span::raw("  "));
        }
        text.push(Line::from(spans));

        // Seventh row: Temporaries T4-T7
        let mut spans = Vec::new();
        for i in 19..=22 {
            let reg_name = format!("T{}", i - 15);
            spans.extend(self.format_register_with_color(&reg_name, i, vm, Color::LightYellow));
            spans.push(Span::raw("  "));
        }
        text.push(Line::from(spans));

        // Eighth row: Special registers (SC, SB, SP, FP, GP)
        let mut spans = Vec::new();
        for i in 27..=31 {
            let reg_name = match i {
                27 => "SC",
                28 => "SB",
                29 => "SP",
                30 => "FP",
                31 => "GP",
                _ => unreachable!(),
            };
            spans.extend(self.format_register_with_color(reg_name, i, vm, Color::LightRed));
            spans.push(Span::raw("  "));
        }
        text.push(Line::from(spans));

        let title = format!(
            " Registers [{}] ",
            if self.focused_pane == FocusedPane::Registers {
                "ACTIVE"
            } else {
                "F2"
            }
        );
        let block = Block::default()
            .title(title)
            .borders(Borders::ALL)
            .border_style(if self.focused_pane == FocusedPane::Registers {
                Style::default().fg(Color::Cyan)
            } else {
                Style::default().fg(Color::Gray)
            });

        let paragraph = Paragraph::new(text).block(block);
        frame.render_widget(paragraph, area);
    }

    #[allow(dead_code)]
    fn format_register(&self, name: &str, index: usize, vm: &VM) -> Vec<Span<'_>> {
        let value = vm.registers[index];
        let was_changed = self.register_changes.contains_key(&index);

        let name_style = if was_changed {
            Style::default()
                .fg(Color::Yellow)
                .add_modifier(Modifier::BOLD)
        } else {
            Style::default().fg(Color::Gray)
        };

        let value_style = if was_changed {
            Style::default()
                .fg(Color::Yellow)
                .add_modifier(Modifier::BOLD)
        } else if value != 0 {
            Style::default().fg(Color::Cyan)
        } else {
            Style::default().fg(Color::DarkGray)
        };

        vec![
            Span::styled(format!("{name:3}"), name_style),
            Span::styled(":", Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{value:04X}"), value_style),
        ]
    }

    fn format_register_with_color(
        &self,
        name: &str,
        index: usize,
        vm: &VM,
        group_color: Color,
    ) -> Vec<Span<'_>> {
        let value = vm.registers[index];
        let was_changed = self.register_changes.contains_key(&index);

        // Register name always stays in its group color
        let name_style = Style::default().fg(group_color);

        // Only the value gets highlighted when changed
        let value_style = if was_changed {
            Style::default()
                .fg(Color::Yellow)
                .add_modifier(Modifier::BOLD)
        } else if value != 0 {
            Style::default().fg(Color::White)
        } else {
            Style::default().fg(Color::DarkGray)
        };

        vec![
            Span::styled(format!("{name:3}"), name_style),
            Span::styled(":", Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{value:04X}"), value_style),
        ]
    }
}
