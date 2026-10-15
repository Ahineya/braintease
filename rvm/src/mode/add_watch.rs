use crate::tui_debugger::{DebuggerMode, MemoryWatch, TuiDebugger, WatchFormat};
use crossterm::event::KeyCode;

impl TuiDebugger {
    pub(crate) fn handle_add_watch_mode(&mut self, key: KeyCode) {
        match key {
            KeyCode::Esc => {
                self.mode = DebuggerMode::Normal;
                self.command_buffer.clear();
            }
            KeyCode::Enter => {
                // Parse format: "name:address[:format]"
                let parts: Vec<&str> = self.command_buffer.split(':').collect();
                if parts.len() >= 2 {
                    let name = parts[0].to_string();
                    if let Ok(addr) = usize::from_str_radix(parts[1].trim_start_matches("0x"), 16) {
                        let format = if parts.len() > 2 {
                            match parts[2] {
                                "hex" | "h" => WatchFormat::Hex,
                                "dec" | "d" => WatchFormat::Decimal,
                                "char" | "c" => WatchFormat::Char,
                                "bin" | "b" => WatchFormat::Binary,
                                _ => WatchFormat::Hex,
                            }
                        } else {
                            WatchFormat::Hex
                        };

                        self.memory_watches.push(MemoryWatch {
                            name,
                            address: addr,
                            size: 1,
                            format,
                        });
                    }
                }
                self.command_buffer.clear();
                self.mode = DebuggerMode::Normal;
            }
            KeyCode::Backspace => {
                self.command_buffer.pop();
            }
            KeyCode::Char(c) => {
                self.command_buffer.push(c);
            }
            _ => {}
        }
    }
}
