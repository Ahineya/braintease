use crate::tui_debugger::{DebuggerMode, TuiDebugger};
use crate::vm::VM;
use crossterm::event::KeyCode;

impl TuiDebugger {
    pub(crate) fn handle_breakpoint_mode(&mut self, key: KeyCode, vm: &VM) {
        match key {
            KeyCode::Esc => {
                self.mode = DebuggerMode::Normal;
                self.command_buffer.clear();
            }
            KeyCode::Enter => {
                let input = self.command_buffer.trim();

                // Try to parse as instruction number first (decimal)
                if let Ok(instr_num) = input.parse::<usize>() {
                    // Convert instruction number to address
                    if instr_num < vm.instructions.len() {
                        let addr = instr_num; // Instruction number is the address
                        if let std::collections::hash_map::Entry::Vacant(e) =
                            self.breakpoints.entry(addr)
                        {
                            e.insert(true); // New breakpoints are enabled by default
                        } else {
                            self.breakpoints.remove(&addr);
                        }
                    }
                } else if input.starts_with("0x") {
                    // Parse as hex address
                    if let Ok(addr) = usize::from_str_radix(&input[2..], 16) {
                        if let std::collections::hash_map::Entry::Vacant(e) =
                            self.breakpoints.entry(addr)
                        {
                            e.insert(true); // New breakpoints are enabled by default
                        } else {
                            self.breakpoints.remove(&addr);
                        }
                    }
                }

                self.command_buffer.clear();
                self.mode = DebuggerMode::Normal;
            }
            KeyCode::Backspace => {
                self.command_buffer.pop();
            }
            KeyCode::Char(c) if c.is_ascii_alphanumeric() || c == 'x' => {
                self.command_buffer.push(c);
            }
            _ => {}
        }
    }
}
