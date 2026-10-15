use crate::tui_debugger::{FocusedPane, TuiDebugger};
use crate::vm::VM;
use ratatui::layout::Rect;
use ratatui::prelude::{Color, Line, Modifier, Span, Style};
use ratatui::widgets::{Block, Borders, List, ListItem, ListState};
use ratatui::Frame;
use ripple_asm::Register;

#[derive(Debug, Clone)]
pub struct CallStackEntry {
    pub from_addr: usize,              // Address where the call was made
    pub to_addr: usize,                // Target address of the call
    pub return_addr: usize,            // Return address stored
    pub is_jal: bool,                  // true for JAL, false for CALL
    pub function_name: Option<String>, // Function name if available from debug symbols
}

impl TuiDebugger {
    pub(crate) fn draw_callstack(&mut self, frame: &mut Frame, area: Rect, vm: &VM) {
        let mut items = Vec::new();

        // Build call stack from execution history
        let callstack = self.build_callstack(vm);

        // Show current location at top
        let pc = vm.registers[Register::Pc as usize] as usize;
        let pcb = vm.registers[Register::Pcb as usize] as usize;
        let current_addr = pcb * vm.bank_size as usize + pc;

        // Add current location
        items.push(ListItem::new(Line::from(vec![
            Span::styled(
                "→ ",
                Style::default()
                    .fg(Color::Yellow)
                    .add_modifier(Modifier::BOLD),
            ),
            Span::styled(
                format!("{:04X}", current_addr),
                Style::default().fg(Color::Yellow),
            ),
            Span::raw(" "),
            self.get_function_name_span(current_addr, vm),
            Span::styled(
                " (current)",
                Style::default()
                    .fg(Color::DarkGray)
                    .add_modifier(Modifier::ITALIC),
            ),
        ])));

        // Add call stack entries
        for (idx, entry) in callstack.iter().enumerate() {
            let mut spans = vec![];

            // Indentation based on depth
            spans.push(Span::raw("  ".repeat(idx.min(5))));

            // Call type indicator
            if entry.is_jal {
                spans.push(Span::styled("JAL ", Style::default().fg(Color::Cyan)));
            } else {
                spans.push(Span::styled("CALL", Style::default().fg(Color::Magenta)));
            }

            // From address
            spans.push(Span::styled(
                format!(" {:04X}", entry.from_addr),
                Style::default().fg(Color::Blue),
            ));

            spans.push(Span::styled(" → ", Style::default().fg(Color::DarkGray)));

            // To address and function name
            spans.push(Span::styled(
                format!("{:04X}", entry.to_addr),
                Style::default().fg(Color::Green),
            ));

            if let Some(ref name) = entry.function_name {
                spans.push(Span::raw(" "));
                spans.push(Span::styled(
                    name.clone(),
                    Style::default()
                        .fg(Color::White)
                        .add_modifier(Modifier::BOLD),
                ));
            }

            // Return address
            spans.push(Span::styled(
                format!(" [ret: {:04X}]", entry.return_addr),
                Style::default().fg(Color::DarkGray),
            ));

            items.push(ListItem::new(Line::from(spans)));
        }

        // If no call stack, show message
        if callstack.is_empty() {
            items.push(ListItem::new(Line::from(vec![Span::styled(
                "  No calls in stack",
                Style::default()
                    .fg(Color::DarkGray)
                    .add_modifier(Modifier::ITALIC),
            )])));
        }

        // Create list widget
        let list = List::new(items).block(
            Block::default()
                .title(format!(
                    " Call Stack [{}] ",
                    if self.focused_pane == FocusedPane::CallStack {
                        "ACTIVE"
                    } else {
                        "F8"
                    }
                ))
                .borders(Borders::ALL)
                .border_style(if self.focused_pane == FocusedPane::CallStack {
                    Style::default().fg(Color::Cyan)
                } else {
                    Style::default().fg(Color::Gray)
                }),
        );

        // Track list state for selection
        let mut state = ListState::default();
        if self.callstack_selected < callstack.len() {
            state.select(Some(self.callstack_selected + 1)); // +1 for current location
        } else {
            state.select(Some(0)); // Select current location
        }

        // Render with state
        frame.render_stateful_widget(list, area, &mut state);
    }

    fn build_callstack(&self, vm: &VM) -> Vec<CallStackEntry> {
        let mut callstack = Vec::new();
        let mut return_addrs = Vec::new();

        // Scan through execution history to build call stack
        // Look for JAL and CALL instructions
        for &addr in self.execution_history.iter().rev() {
            if addr < vm.instructions.len() {
                let inst = &vm.instructions[addr];

                // Check if this is a JAL instruction (opcode 0x13)
                if inst.opcode == 0x13 {
                    let target = inst.word3 as usize;
                    let return_addr = addr + 1;

                    // Check if we've returned from this call
                    if !return_addrs.contains(&return_addr) {
                        callstack.push(CallStackEntry {
                            from_addr: addr,
                            to_addr: target,
                            return_addr,
                            is_jal: true,
                            function_name: self.get_function_name(target, vm),
                        });
                        return_addrs.push(return_addr);
                    }
                }
                // Check for CALL macro (expanded JAL pattern)
                // CALL is typically: JAL RA, target
                else if inst.opcode == 0x13 && inst.word1 == Register::Ra as u16 {
                    let target = inst.word3 as usize;
                    let return_addr = addr + 1;

                    if !return_addrs.contains(&return_addr) {
                        callstack.push(CallStackEntry {
                            from_addr: addr,
                            to_addr: target,
                            return_addr,
                            is_jal: false,
                            function_name: self.get_function_name(target, vm),
                        });
                        return_addrs.push(return_addr);
                    }
                }
            }

            // Limit call stack depth to prevent UI overflow
            if callstack.len() >= 20 {
                break;
            }
        }

        callstack
    }

    fn get_function_name(&self, addr: usize, vm: &VM) -> Option<String> {
        // Look up function name from debug symbols
        // The HashMap is keyed by address with values as function names
        vm.debug_symbols.get(&addr).cloned()
    }

    fn get_function_name_span(&self, addr: usize, vm: &VM) -> Span<'static> {
        if let Some(name) = self.get_function_name(addr, vm) {
            Span::styled(
                name,
                Style::default()
                    .fg(Color::White)
                    .add_modifier(Modifier::BOLD),
            )
        } else {
            Span::styled("(unknown)", Style::default().fg(Color::DarkGray))
        }
    }

    pub(crate) fn navigate_to_callstack_entry(&mut self, vm: &VM) {
        let callstack = self.build_callstack(vm);

        if self.callstack_selected == 0 {
            // Navigate to current PC
            let pc = vm.registers[Register::Pc as usize] as usize;
            let pcb = vm.registers[Register::Pcb as usize] as usize;
            let addr = pcb * vm.bank_size as usize + pc;
            self.disasm_scroll = addr.saturating_sub(5);
        } else if self.callstack_selected <= callstack.len() {
            // Navigate to selected call
            let entry = &callstack[self.callstack_selected - 1];
            self.disasm_scroll = entry.from_addr.saturating_sub(5);
        }

        // Switch focus to disassembly
        self.focused_pane = FocusedPane::Disassembly;
    }
}
