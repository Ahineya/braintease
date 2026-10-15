use crate::tui_debugger::{FocusedPane, TuiDebugger};
use crate::vm::VM;
use ratatui::layout::Rect;
use ratatui::prelude::{Color, Line, Modifier, Span, Style};
use ratatui::widgets::{Block, Borders, Paragraph};
use ratatui::Frame;
use ripple_asm::Register;

impl TuiDebugger {
    pub(crate) fn draw_stack(&self, frame: &mut Frame, area: Rect, vm: &VM) {
        let mut text = Vec::new();

        // Get stack registers
        let sp = vm.registers[Register::Sp as usize];
        let sb = vm.registers[Register::Sb as usize];
        let fp = vm.registers[Register::Fp as usize];

        // Show stack info at top
        let mut stack_info = vec![
            Span::raw("SP: "),
            Span::styled(format!("{sp:04X}"), Style::default().fg(Color::Yellow)),
        ];
        if sp > 0 {
            stack_info.push(Span::styled(
                format!(" (TOS@{:04X})", sp - 1),
                Style::default().fg(Color::DarkGray),
            ));
        }
        stack_info.push(Span::raw(" FP: "));
        stack_info.push(Span::styled(
            format!("{fp:04X}"),
            Style::default().fg(Color::Cyan),
        ));
        stack_info.push(Span::raw(" SB: "));
        stack_info.push(Span::styled(
            format!("{sb:04X}"),
            Style::default().fg(Color::Green),
        ));
        text.push(Line::from(stack_info));

        // Show return address
        text.push(Line::from(vec![
            Span::raw("Return: "),
            Span::styled(
                format!(
                    "{:04X}:{:04X}",
                    vm.registers[Register::Rab as usize],
                    vm.registers[Register::Ra as usize]
                ),
                Style::default().fg(Color::Magenta),
            ),
        ]));
        text.push(Line::from(""));

        // Calculate actual stack memory address
        let stack_base_addr = sb as usize * vm.bank_size as usize;

        // Calculate visible area for stack
        let visible_lines = area.height.saturating_sub(6) as usize; // Account for title, stack info, etc.

        // SP points to the next free slot, so we want to show from SP-1 downwards
        // But also show 5 addresses beyond SP to see what's ahead
        const LOOKAHEAD: usize = 5;

        // Extended size includes SP position + lookahead
        let stack_size = if sp > 0 { sp as usize } else { 0 };
        let extended_size = stack_size + LOOKAHEAD;

        if sp == 0 || sb == 0 {
            text.push(Line::from(Span::styled(
                "Stack not initialized",
                Style::default().fg(Color::DarkGray),
            )));
        } else {
            // Calculate the range of stack entries to show
            // We'll show the stack growing upward, with most recent (SP-1) at top
            // Plus some lookahead beyond SP

            // Calculate scroll bounds using extended size
            let total_items = extended_size;
            let max_scroll = total_items.saturating_sub(visible_lines);
            let scroll = self.stack_scroll.min(max_scroll);

            // Show from newest (including lookahead) down to oldest
            let start_offset = scroll;
            let end_offset = (start_offset + visible_lines).min(extended_size);

            if stack_size == 0 {
                text.push(Line::from(Span::styled(
                    "Stack is empty",
                    Style::default().fg(Color::DarkGray),
                )));
            } else {
                // Determine the initial stack base value (usually 1000 in decimal)
                // This is where SP and FP start according to crt0.asm
                let stack_base_value = 1000u16; // Initial SP/FP value from crt0.asm

                for offset in start_offset..end_offset {
                    // Show stack from top (SP+lookahead) down to bottom
                    // We want to display most recent pushes at the top
                    let display_idx = extended_size - 1 - offset;

                    // The actual memory address is stack_base_addr + display_idx
                    // But display_idx is the absolute stack position (e.g., 1002)
                    // We want to show the offset from the initial stack base
                    let stack_position = display_idx as u16; // This will be values like 1000, 1001, 1002...
                    let stack_offset = stack_position.saturating_sub(stack_base_value);

                    let mem_addr = stack_base_addr + display_idx;

                    let mut spans = vec![];

                    // Mark current positions - combine markers when they overlap
                    // Note: SP points to next free slot, so last value is at SP-1
                    let is_sp = display_idx == sp as usize;
                    let is_fp = display_idx == fp as usize;
                    let is_tos = sp > 0 && display_idx == (sp as usize - 1);
                    let is_beyond = display_idx > sp as usize;

                    // Build marker string based on what's at this position
                    let marker = if is_sp && is_fp {
                        Span::styled("S+F", Style::default().fg(Color::Magenta))
                    // Both SP and FP
                    } else if is_tos && is_fp {
                        Span::styled("T+F", Style::default().fg(Color::Magenta))
                    // TOS and FP
                    } else if is_sp {
                        Span::styled("SP→", Style::default().fg(Color::Green)) // Stack Pointer (next free)
                    } else if is_fp {
                        Span::styled("FP→", Style::default().fg(Color::Cyan)) // Frame Pointer
                    } else if is_tos {
                        Span::styled("TOS", Style::default().fg(Color::Yellow)) // Top of Stack
                    } else if is_beyond {
                        Span::styled("   ", Style::default().fg(Color::DarkGray))
                    // Beyond SP
                    } else {
                        Span::raw("   ")
                    };

                    spans.push(marker);

                    // Show stack offset from initial base (e.g., +0, +1, +2...)
                    // Color differently if beyond SP
                    let offset_color = if display_idx >= sp as usize {
                        Color::DarkGray // Beyond SP - future stack space
                    } else {
                        Color::Blue // Active stack
                    };
                    spans.push(Span::styled(
                        format!("[+{stack_offset:03X}]"),
                        Style::default().fg(offset_color),
                    ));

                    // Also show the actual stack position for debugging
                    spans.push(Span::styled(
                        format!(" {stack_position:04X} "),
                        Style::default().fg(Color::Gray),
                    ));

                    // Show memory value at this stack location
                    if mem_addr < vm.memory.len() {
                        let value = vm.memory[mem_addr];
                        let value_color = if display_idx >= sp as usize {
                            Color::DarkGray // Beyond SP - dim the value
                        } else {
                            Color::White // Active stack value
                        };
                        spans.push(Span::styled(
                            format!("{value:04X}"),
                            Style::default().fg(value_color),
                        ));

                        // Show ASCII representation if printable
                        if (0x20..=0x7E).contains(&value) {
                            spans.push(Span::styled(
                                format!(" '{}'", value as u8 as char),
                                Style::default().fg(Color::Green),
                            ));
                        }

                        // Try to detect if this looks like a return address
                        // (values that could be valid code addresses)
                        if value < vm.instructions.len() as u16 {
                            spans.push(Span::styled(
                                " (code?)",
                                Style::default()
                                    .fg(Color::Magenta)
                                    .add_modifier(Modifier::ITALIC),
                            ));
                        }
                    } else {
                        spans.push(Span::styled("????", Style::default().fg(Color::Red)));
                    }

                    text.push(Line::from(spans));
                }
            }
        }

        let scroll_indicator = if extended_size > visible_lines {
            format!(" [{}/{}]", self.stack_scroll + 1, extended_size)
        } else {
            String::new()
        };

        let title = format!(
            " Stack{} [{}] ",
            scroll_indicator,
            if self.focused_pane == FocusedPane::Stack {
                "ACTIVE"
            } else {
                "F4"
            }
        );
        let block = Block::default()
            .title(title)
            .borders(Borders::ALL)
            .border_style(if self.focused_pane == FocusedPane::Stack {
                Style::default().fg(Color::Cyan)
            } else {
                Style::default().fg(Color::Gray)
            });

        let paragraph = Paragraph::new(text).block(block);
        frame.render_widget(paragraph, area);
    }
}
