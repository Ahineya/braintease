use crate::tui_debugger::{DebuggerMode, TuiDebugger};
use crossterm::event::KeyCode;

impl TuiDebugger {
    pub(crate) fn handle_goto_mode(&mut self, key: KeyCode) {
        match key {
            KeyCode::Esc => {
                self.mode = DebuggerMode::Normal;
                self.command_buffer.clear();
            }
            KeyCode::Enter => {
                // Parse address - handle both "0x" prefix and plain hex
                let addr_str = self.command_buffer.trim();
                let addr_result = if addr_str.starts_with("0x") || addr_str.starts_with("0X") {
                    usize::from_str_radix(&addr_str[2..], 16)
                } else {
                    usize::from_str_radix(addr_str, 16)
                };

                if let Ok(addr) = addr_result {
                    self.memory_base_addr = addr & !0xF; // Align to 16 bytes
                    self.memory_scroll = 0;
                }
                self.command_buffer.clear();
                self.mode = DebuggerMode::Normal;
            }
            KeyCode::Backspace => {
                self.command_buffer.pop();
            }
            KeyCode::Char(c) if c.is_ascii_hexdigit() || c == 'x' => {
                self.command_buffer.push(c);
            }
            _ => {}
        }
    }
}
