use crossterm::event::KeyCode;
use crate::tui_debugger::{DebuggerMode, MemoryWatch, TuiDebugger, WatchFormat};
use crate::vm::VM;

impl TuiDebugger {
    pub(crate) fn handle_command_mode(&mut self, key: KeyCode, vm: &mut VM) -> bool {
        match key {
            KeyCode::Esc => {
                self.mode = DebuggerMode::Normal;
                self.command_buffer.clear();
            }
            KeyCode::Enter => {
                let command = self.command_buffer.clone();
                let should_continue = self.execute_command(&command, vm);
                if !should_continue {
                    return false; // Quit the debugger_ui
                }
                self.command_history.push(command);
                self.command_buffer.clear();
                self.mode = DebuggerMode::Normal;
            }
            KeyCode::Backspace => {
                self.command_buffer.pop();
            }
            KeyCode::Char(c) => {
                self.command_buffer.push(c);
            }
            KeyCode::Up => {
                if self.command_history_idx > 0 {
                    self.command_history_idx -= 1;
                    self.command_buffer = self.command_history[self.command_history_idx].clone();
                }
            }
            KeyCode::Down => {
                if self.command_history_idx < self.command_history.len() - 1 {
                    self.command_history_idx += 1;
                    self.command_buffer = self.command_history[self.command_history_idx].clone();
                }
            }
            _ => {}
        }
        true // Continue unless quit command was entered
    }

    pub(crate) fn execute_command(&mut self, command: &str, vm: &mut VM) -> bool {
        let parts: Vec<&str> = command.split_whitespace().collect();
        if parts.is_empty() {
            return true;
        }

        match parts[0] {
            // Breakpoint commands
            "b" | "break" => {
                // Usage: break <addr>  - Set breakpoint.rs at address
                if parts.len() > 1 {
                    if let Ok(addr) = usize::from_str_radix(parts[1].trim_start_matches("0x"), 16) {
                        self.breakpoints.insert(addr);
                    }
                }
            }
            "d" | "delete" => {
                // Usage: delete <addr> - Remove breakpoint.rs at address
                if parts.len() > 1 {
                    if let Ok(addr) = usize::from_str_radix(parts[1].trim_start_matches("0x"), 16) {
                        self.breakpoints.remove(&addr);
                    }
                }
            }

            // Watch commands
            "w" | "watch" => {
                // Usage: watch <name> <addr> - Add memory watch
                if parts.len() > 2 {
                    let name = parts[1].to_string();
                    if let Ok(addr) = usize::from_str_radix(parts[2].trim_start_matches("0x"), 16) {
                        self.memory_watches.push(MemoryWatch {
                            name,
                            address: addr,
                            size: 1,
                            format: WatchFormat::Hex,
                        });
                    }
                }
            }

            // Memory commands
            "m" | "mem" => {
                // Usage: mem <addr> <value> - Write value to memory
                if parts.len() > 2 {
                    if let Ok(addr) = usize::from_str_radix(parts[1].trim_start_matches("0x"), 16) {
                        if let Ok(value) = u16::from_str_radix(parts[2].trim_start_matches("0x"), 16) {
                            if addr < vm.memory.len() {
                                vm.memory[addr] = value;
                            }
                        }
                    }
                }
            }

            // Register commands
            "reg" => {
                // Usage: reg <reg#> <value> - Set register value
                if parts.len() > 2 {
                    if let Ok(reg) = parts[1].parse::<usize>() {
                        if let Ok(value) = u16::from_str_radix(parts[2].trim_start_matches("0x"), 16) {
                            if reg < 18 {
                                vm.registers[reg] = value;
                            }
                        }
                    }
                }
            }

            // Help command
            "help" | "h" | "?" => {
                self.show_help = true;
            }

            // Quit command
            "q" | "quit" | "exit" => {
                return false; // Signal to quit
            }

            _ => {}
        }

        true // Continue running
    }
    
}