use crate::tui_debugger::{DebuggerMode, TuiDebugger};
use crate::vm::VM;
use crossterm::event::KeyCode;

impl TuiDebugger {
    pub(crate) fn handle_memory_edit_mode(&mut self, key: KeyCode, vm: &mut VM) {
        match key {
            KeyCode::Esc => {
                self.mode = DebuggerMode::Normal;
                self.command_buffer.clear();
            }
            KeyCode::Enter => {
                // Check if this is instruction editing (starts with 'i')
                if self.command_buffer.starts_with('i') {
                    // Format: i<instr_idx>:<byte_idx>:<hex_value>
                    let parts: Vec<&str> = self.command_buffer[1..].split(':').collect();
                    if parts.len() >= 3 {
                        if let (Ok(instr_idx), Ok(byte_idx), Ok(value)) = (
                            usize::from_str_radix(parts[0], 16),
                            parts[1].parse::<usize>(),
                            u8::from_str_radix(parts[2], 16),
                        ) {
                            if instr_idx < vm.instructions.len() && byte_idx < 8 {
                                let instr = &mut vm.instructions[instr_idx];
                                match byte_idx {
                                    0 => instr.opcode = value,
                                    1 => instr.word0 = value,
                                    2 => instr.word1 = (instr.word1 & 0xFF00) | value as u16,
                                    3 => {
                                        instr.word1 = (instr.word1 & 0x00FF) | ((value as u16) << 8)
                                    }
                                    4 => instr.word2 = (instr.word2 & 0xFF00) | value as u16,
                                    5 => {
                                        instr.word2 = (instr.word2 & 0x00FF) | ((value as u16) << 8)
                                    }
                                    6 => instr.word3 = (instr.word3 & 0xFF00) | value as u16,
                                    7 => {
                                        instr.word3 = (instr.word3 & 0x00FF) | ((value as u16) << 8)
                                    }
                                    _ => {}
                                }
                            }
                        }
                    }
                    self.command_buffer.clear();
                    self.mode = DebuggerMode::Normal;
                    return;
                }

                // Parse regular memory edit: "address:value" or just "value" if address is pre-filled
                let parts: Vec<&str> = self.command_buffer.split(':').collect();

                let (addr_str, value_str) = if parts.len() == 2 {
                    (parts[0], parts[1])
                } else if parts.len() == 1 && self.command_buffer.contains(':') {
                    // Address pre-filled, just value after colon
                    let colon_pos = self.command_buffer.find(':').unwrap();
                    let (addr_part, value_part) = self.command_buffer.split_at(colon_pos);
                    (addr_part, &value_part[1..])
                } else {
                    // Invalid format
                    self.command_buffer.clear();
                    self.mode = DebuggerMode::Normal;
                    return;
                };

                if let Ok(mut addr) = usize::from_str_radix(addr_str.trim_start_matches("0x"), 16) {
                    // Check if it's a string literal
                    if value_str.starts_with('"') && value_str.ends_with('"') {
                        // String literal - write multiple bytes
                        let string_content = &value_str[1..value_str.len() - 1];
                        for ch in string_content.chars() {
                            if addr < vm.memory.len() {
                                vm.memory[addr] = ch as u16;
                                addr += 1;
                            }
                        }
                        // Jump to the modified address for visual feedback
                        if let Ok(start_addr) =
                            usize::from_str_radix(addr_str.trim_start_matches("0x"), 16)
                        {
                            self.memory_base_addr = start_addr & !0xF;
                            self.memory_scroll = 0;
                        }
                    } else {
                        // Parse single value - support multiple formats
                        let value = if value_str.starts_with("'")
                            && value_str.ends_with("'")
                            && value_str.len() == 3
                        {
                            // Character literal
                            value_str.chars().nth(1).map(|c| c as u16)
                        } else if value_str.starts_with("0x") {
                            // Hexadecimal
                            u16::from_str_radix(&value_str[2..], 16).ok()
                        } else if value_str.starts_with("0b") {
                            // Binary
                            u16::from_str_radix(&value_str[2..], 2).ok()
                        } else if value_str.chars().all(|c| c.is_ascii_hexdigit()) {
                            // Assume hex without 0x prefix
                            u16::from_str_radix(value_str, 16).ok()
                        } else {
                            // Decimal
                            value_str.parse::<u16>().ok()
                        };

                        if let Some(val) = value {
                            if addr < vm.memory.len() {
                                vm.memory[addr] = val;
                                // Don't change the memory view position when editing
                                // User can manually navigate if needed
                            }
                        }
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
