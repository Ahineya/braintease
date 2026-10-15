use crate::tui_debugger::{DebuggerMode, FocusedPane, TuiDebugger, MEMORY_NAV_COLS};
use crate::vm::{VMState, VM};
use crossterm::event::{KeyCode, KeyModifiers};

impl TuiDebugger {
    pub(crate) fn handle_normal_mode(
        &mut self,
        key: KeyCode,
        modifiers: KeyModifiers,
        vm: &mut VM,
    ) -> bool {
        match key {
            // Quit
            KeyCode::Char('q') if modifiers == KeyModifiers::NONE => return false,
            KeyCode::Char('Q') if modifiers == KeyModifiers::SHIFT => return false,

            // Escape key - close help if open, otherwise do nothing
            KeyCode::Esc => {
                if self.show_help {
                    self.show_help = false;
                }
            }

            // Help
            KeyCode::Char('?') => {
                self.show_help = !self.show_help;
                if self.show_help {
                    self.help_scroll = 0; // Reset scroll when opening
                }
            }

            // Execution control
            KeyCode::Char(' ') | KeyCode::Char('s') => {
                // Special handling for Breakpoints panel - toggle enable/disable
                if self.focused_pane == FocusedPane::Breakpoints {
                    self.toggle_selected_breakpoint();
                } else {
                    // When stepping manually, we don't need to check for breakpoints
                    // If we're at a breakpoint state, clear it first
                    if matches!(vm.state, VMState::Breakpoint) {
                        vm.state = VMState::Running;
                    }
                    self.step_vm_no_break_check(vm);
                }
            }
            KeyCode::Char('r') => {
                // If at breakpoint.rs, clear state first
                if matches!(vm.state, VMState::Breakpoint) {
                    vm.state = VMState::Running;
                }
                self.run_until_break(vm);
            }
            KeyCode::Char('c') => {
                if matches!(vm.state, VMState::Breakpoint) {
                    vm.state = VMState::Running;
                    // Step once to get past the current breakpoint.rs
                    self.step_vm_no_break_check(vm);
                    // Then continue running
                    if matches!(vm.state, VMState::Running) {
                        self.run_until_break(vm);
                    }
                }
            }

            // Pane switching
            KeyCode::F(1) => self.focused_pane = FocusedPane::Disassembly,
            KeyCode::F(2) => self.focused_pane = FocusedPane::Registers,
            KeyCode::F(3) => self.focused_pane = FocusedPane::Memory,
            KeyCode::F(4) => self.focused_pane = FocusedPane::Stack,
            KeyCode::F(5) => self.focused_pane = FocusedPane::Watches,
            KeyCode::F(6) => self.focused_pane = FocusedPane::Breakpoints,
            KeyCode::F(7) => self.focused_pane = FocusedPane::Output,
            KeyCode::F(8) => self.focused_pane = FocusedPane::CallStack,
            KeyCode::Tab if modifiers == KeyModifiers::NONE => self.cycle_pane(),
            KeyCode::BackTab | KeyCode::Tab if modifiers == KeyModifiers::SHIFT => {
                self.cycle_pane_reverse()
            }

            // Navigation based on focused pane
            KeyCode::Up | KeyCode::Char('k') => {
                // Special case: navigate call stack
                if self.focused_pane == FocusedPane::CallStack {
                    if self.callstack_selected > 0 {
                        self.callstack_selected -= 1;
                    }
                } else {
                    if self.show_help {
                        self.help_scroll = self.help_scroll.saturating_sub(1);
                    } else {
                        self.navigate_up(vm);
                    }
                }
            }
            KeyCode::Down | KeyCode::Char('j') => {
                // Special case: navigate call stack
                if self.focused_pane == FocusedPane::CallStack {
                    self.callstack_selected += 1; // Will be clamped when drawing
                } else if self.show_help {
                    self.help_scroll = self.help_scroll.saturating_add(1);
                } else {
                    self.navigate_down(vm);
                }
            }
            KeyCode::Left | KeyCode::Char('h') => self.navigate_left(vm),
            KeyCode::Right | KeyCode::Char('l') => self.navigate_right(vm),

            // Enter key - special handling for call stack
            KeyCode::Enter if self.focused_pane == FocusedPane::CallStack => {
                self.navigate_to_callstack_entry(vm);
            }
            KeyCode::PageUp => self.page_up(vm),
            KeyCode::PageDown => self.page_down(vm),

            // Breakpoints
            KeyCode::Char('b') if modifiers == KeyModifiers::NONE => {
                self.toggle_breakpoint_at_cursor(vm)
            }
            KeyCode::Char('B') if modifiers == KeyModifiers::SHIFT => {
                // Enter breakpoint.rs mode to set/toggle breakpoint.rs by instruction number
                self.mode = DebuggerMode::SetBreakpoint;
                self.command_buffer.clear();
            }
            KeyCode::Char('B') if modifiers == KeyModifiers::NONE => self.breakpoints.clear(),

            // Breakpoint panel operations when focused
            KeyCode::Char('d') | KeyCode::Delete
                if self.focused_pane == FocusedPane::Breakpoints =>
            {
                self.delete_selected_breakpoint();
            }

            // Memory operations
            KeyCode::Char('g') if modifiers == KeyModifiers::NONE => {
                self.mode = DebuggerMode::GotoAddress
            }
            KeyCode::Char('G') if modifiers == KeyModifiers::SHIFT => {
                // Quick jump to stack
                let sb = vm.registers[ripple_asm::Register::Sb as usize];
                let sp = vm.registers[ripple_asm::Register::Sp as usize];
                let stack_addr =
                    sb as usize * vm.bank_size as usize + sp.saturating_sub(5) as usize;
                self.memory_base_addr = stack_addr & !0x7; // Align to 8-byte boundary
                self.memory_scroll = 0;
                self.focused_pane = FocusedPane::Memory;
            }
            KeyCode::Char('w') => self.mode = DebuggerMode::AddWatch,
            KeyCode::Char('W') => self.remove_selected_watch(),
            KeyCode::Char('e') => {
                if self.focused_pane == FocusedPane::Memory {
                    // Pre-fill with current cursor position
                    let addr = self.memory_base_addr
                        + self.memory_scroll * MEMORY_NAV_COLS
                        + self.memory_cursor_col;
                    self.command_buffer = format!("{addr:04x}:");
                    self.mode = DebuggerMode::MemoryEdit;
                } else if self.focused_pane == FocusedPane::Disassembly && self.show_instruction_hex
                {
                    // Edit instruction byte at cursor
                    let instr_idx = self.disasm_scroll + self.disasm_cursor_row;
                    if instr_idx < vm.instructions.len() {
                        self.command_buffer =
                            format!("i{:04x}:{}:", instr_idx, self.disasm_cursor_byte);
                        self.mode = DebuggerMode::MemoryEdit;
                    }
                } else {
                    self.mode = DebuggerMode::MemoryEdit;
                }
            }

            // Panel visibility toggles (using Alt+number to match F-keys)
            KeyCode::Char('1') if modifiers == KeyModifiers::ALT => {
                // Disassembly - can't be hidden
            }
            KeyCode::Char('2') if modifiers == KeyModifiers::ALT => {
                self.show_registers = !self.show_registers;
            }
            KeyCode::Char('3') if modifiers == KeyModifiers::ALT => {
                self.show_memory = !self.show_memory;
            }
            KeyCode::Char('4') if modifiers == KeyModifiers::ALT => {
                self.show_stack = !self.show_stack;
            }
            KeyCode::Char('5') if modifiers == KeyModifiers::ALT => {
                self.show_watches = !self.show_watches;
            }
            KeyCode::Char('6') if modifiers == KeyModifiers::ALT => {
                self.show_breakpoints = !self.show_breakpoints;
            }
            KeyCode::Char('7') if modifiers == KeyModifiers::ALT => {
                self.show_output = !self.show_output;
            }
            KeyCode::Char('8') if modifiers == KeyModifiers::ALT => {
                self.show_callstack = !self.show_callstack;
            }

            // Toggle hex view in disassembly
            KeyCode::Char('H') if modifiers == KeyModifiers::SHIFT => {
                if self.focused_pane == FocusedPane::Disassembly {
                    self.show_instruction_hex = !self.show_instruction_hex;
                    if self.show_instruction_hex {
                        // Reset cursor when enabling hex view
                        self.disasm_cursor_row = 0;
                        self.disasm_cursor_byte = 0;
                    }
                }
            }

            // Toggle debug symbols in disassembly
            KeyCode::Char('D') if modifiers == KeyModifiers::SHIFT => {
                self.show_debug_symbols = !self.show_debug_symbols;
            }

            // Alternative toggle shortcuts using 'T' for toggle mode
            KeyCode::Char('T') if modifiers == KeyModifiers::SHIFT => {
                // Enter panel toggle mode - next key will toggle a panel
                self.mode = DebuggerMode::Command; // Reuse command mode temporarily
                self.command_buffer = "toggle:".to_string();
            }

            // Command mode
            KeyCode::Char(':') => {
                self.mode = DebuggerMode::Command;
                self.command_buffer.clear();
            }

            // Reset
            KeyCode::Char('R') => {
                vm.reset();
                self.execution_history.clear();
                self.register_changes.clear();
            }

            // Toggle ASCII display in memory view
            KeyCode::Char('a') if self.focused_pane == FocusedPane::Memory => {
                self.show_ascii = !self.show_ascii;
            }

            // Jump to previous/next memory bank (works globally)
            KeyCode::Char('[') => {
                // Jump to previous bank
                let current_bank = self.memory_base_addr / vm.bank_size as usize;
                if current_bank > 0 {
                    self.memory_base_addr = (current_bank - 1) * vm.bank_size as usize;
                    self.memory_scroll = 0;
                    self.memory_cursor_col = 0;
                    // Switch to memory pane to show the change
                    self.focused_pane = FocusedPane::Memory;
                }
            }
            KeyCode::Char(']') => {
                // Jump to next bank
                let current_bank = self.memory_base_addr / vm.bank_size as usize;
                let next_bank_start = (current_bank + 1) * vm.bank_size as usize;
                if next_bank_start < vm.memory.len() {
                    self.memory_base_addr = next_bank_start;
                    self.memory_scroll = 0;
                    self.memory_cursor_col = 0;
                    // Switch to memory pane to show the change
                    self.focused_pane = FocusedPane::Memory;
                }
            }

            // Quick memory edit - if in memory view and pressing hex digit
            KeyCode::Char(c)
                if self.focused_pane == FocusedPane::Memory && c.is_ascii_hexdigit() =>
            {
                let addr = self.memory_base_addr
                    + self.memory_scroll * MEMORY_NAV_COLS
                    + self.memory_cursor_col;
                self.command_buffer = format!("{addr:04x}:{c}");
                self.mode = DebuggerMode::MemoryEdit;
            }

            // Quick instruction edit - if in disassembly hex view and pressing hex digit
            KeyCode::Char(c)
                if self.focused_pane == FocusedPane::Disassembly
                    && self.show_instruction_hex
                    && c.is_ascii_hexdigit() =>
            {
                let instr_idx = self.disasm_scroll + self.disasm_cursor_row;
                // Start editing this instruction byte
                self.command_buffer =
                    format!("i{:04x}:{}:{}", instr_idx, self.disasm_cursor_byte, c);
                self.mode = DebuggerMode::MemoryEdit; // Reuse memory edit mode for instructions
            }

            _ => {}
        }

        true
    }

    pub(crate) fn toggle_breakpoint_at_cursor(&mut self, vm: &VM) {
        if self.focused_pane == FocusedPane::Disassembly {
            // Use the actual cursor position, not just the scroll position
            let addr = self.disasm_scroll + self.disasm_cursor_row;
            // Make sure we're within valid instruction range
            if addr < vm.instructions.len() {
                if let std::collections::hash_map::Entry::Vacant(e) = self.breakpoints.entry(addr) {
                    e.insert(true); // New breakpoints are enabled by default
                } else {
                    self.breakpoints.remove(&addr);
                }
            }
        }
    }

    pub(crate) fn remove_selected_watch(&mut self) {
        if !self.memory_watches.is_empty() && self.selected_watch < self.memory_watches.len() {
            self.memory_watches.remove(self.selected_watch);
            if self.selected_watch > 0 && self.selected_watch >= self.memory_watches.len() {
                self.selected_watch -= 1;
            }
        }
    }

    pub(crate) fn toggle_selected_breakpoint(&mut self) {
        if !self.breakpoints.is_empty() {
            // Get sorted breakpoints to find the selected one
            let mut sorted_breakpoints: Vec<_> = self.breakpoints.keys().cloned().collect();
            sorted_breakpoints.sort();

            if self.selected_breakpoint < sorted_breakpoints.len() {
                let addr = sorted_breakpoints[self.selected_breakpoint];
                // Toggle the enabled state
                if let Some(enabled) = self.breakpoints.get_mut(&addr) {
                    *enabled = !*enabled;
                }
            }
        }
    }

    pub(crate) fn delete_selected_breakpoint(&mut self) {
        if !self.breakpoints.is_empty() {
            // Get sorted breakpoints to find the selected one
            let mut sorted_breakpoints: Vec<_> = self.breakpoints.keys().cloned().collect();
            sorted_breakpoints.sort();

            if self.selected_breakpoint < sorted_breakpoints.len() {
                let addr = sorted_breakpoints[self.selected_breakpoint];
                self.breakpoints.remove(&addr);

                // Adjust selected index if needed
                if self.selected_breakpoint > 0
                    && self.selected_breakpoint >= self.breakpoints.len()
                {
                    self.selected_breakpoint -= 1;
                }
            }
        }
    }

    pub(crate) fn cycle_pane(&mut self) {
        self.focused_pane = match self.focused_pane {
            FocusedPane::Disassembly => FocusedPane::Registers,
            FocusedPane::Registers => FocusedPane::Memory,
            FocusedPane::Memory => FocusedPane::Stack,
            FocusedPane::Stack => FocusedPane::CallStack,
            FocusedPane::CallStack => FocusedPane::Watches,
            FocusedPane::Watches => FocusedPane::Breakpoints,
            FocusedPane::Breakpoints => FocusedPane::Output,
            FocusedPane::Output => FocusedPane::Disassembly,
            _ => FocusedPane::Disassembly,
        };
    }

    pub(crate) fn cycle_pane_reverse(&mut self) {
        self.focused_pane = match self.focused_pane {
            FocusedPane::Disassembly => FocusedPane::Output,
            FocusedPane::Registers => FocusedPane::Disassembly,
            FocusedPane::Memory => FocusedPane::Registers,
            FocusedPane::Stack => FocusedPane::Memory,
            FocusedPane::CallStack => FocusedPane::Stack,
            FocusedPane::Watches => FocusedPane::CallStack,
            FocusedPane::Breakpoints => FocusedPane::Watches,
            FocusedPane::Output => FocusedPane::Breakpoints,
            _ => FocusedPane::Output,
        };
    }

    pub(crate) fn navigate_up(&mut self, _vm: &VM) {
        match self.focused_pane {
            FocusedPane::Disassembly => {
                if self.show_instruction_hex {
                    // Navigate within hex view
                    if self.disasm_cursor_row > 0 {
                        self.disasm_cursor_row -= 1;
                    } else if self.disasm_scroll > 0 {
                        self.disasm_scroll -= 1;
                    }
                } else {
                    self.disasm_scroll = self.disasm_scroll.saturating_sub(1);
                }
            }
            FocusedPane::Memory => {
                // Move up by one row
                if self.memory_scroll > 0 {
                    self.memory_scroll -= 1;
                } else if self.memory_base_addr >= MEMORY_NAV_COLS {
                    // Need to scroll the base address up
                    self.memory_base_addr -= MEMORY_NAV_COLS;
                }
            }
            FocusedPane::Output => {
                self.output_scroll = self.output_scroll.saturating_sub(1);
            }
            FocusedPane::Stack => {
                self.stack_scroll = self.stack_scroll.saturating_sub(1);
            }
            FocusedPane::Watches => {
                if self.selected_watch > 0 {
                    self.selected_watch -= 1;
                    // Auto-scroll will happen in draw_watches
                }
            }
            FocusedPane::Breakpoints => {
                if self.selected_breakpoint > 0 {
                    self.selected_breakpoint -= 1;
                    // Auto-scroll will happen in draw_breakpoints
                }
            }
            _ => {}
        }
    }

    pub(crate) fn navigate_down(&mut self, vm: &VM) {
        match self.focused_pane {
            FocusedPane::Disassembly => {
                if self.show_instruction_hex {
                    // Navigate within hex view
                    let visible_lines = 20; // Approximate
                    if self.disasm_cursor_row < visible_lines - 1
                        && self.disasm_scroll + self.disasm_cursor_row < vm.instructions.len() - 1
                    {
                        self.disasm_cursor_row += 1;
                    } else if self.disasm_scroll < vm.instructions.len().saturating_sub(1) {
                        self.disasm_scroll += 1;
                    }
                } else if self.disasm_scroll < vm.instructions.len().saturating_sub(1) {
                    self.disasm_scroll += 1;
                }
            }
            FocusedPane::Memory => {
                // Move down by one row
                let next_row_addr =
                    self.memory_base_addr + (self.memory_scroll + 1) * MEMORY_NAV_COLS;
                if next_row_addr < vm.memory.len() {
                    self.memory_scroll += 1;
                    // Check if we need to scroll the view
                    let visible_rows = 15; // Approximate visible rows
                    if self.memory_scroll >= visible_rows {
                        self.memory_base_addr += MEMORY_NAV_COLS;
                        self.memory_scroll -= 1;
                    }
                }
            }
            FocusedPane::Output => {
                self.output_scroll += 1;
            }
            FocusedPane::Stack => {
                // Allow scrolling through execution history
                if self.stack_scroll < self.execution_history.len().saturating_sub(1) {
                    self.stack_scroll += 1;
                }
            }
            FocusedPane::Watches => {
                if self.selected_watch < self.memory_watches.len().saturating_sub(1) {
                    self.selected_watch += 1;
                    // Auto-scroll will happen in draw_watches
                }
            }
            FocusedPane::Breakpoints => {
                // Get sorted breakpoints count
                let breakpoint_count = self.breakpoints.len();
                if self.selected_breakpoint < breakpoint_count.saturating_sub(1) {
                    self.selected_breakpoint += 1;
                    // Auto-scroll will happen in draw_breakpoints
                }
            }
            _ => {}
        }
    }

    pub(crate) fn navigate_left(&mut self, _vm: &VM) {
        match self.focused_pane {
            FocusedPane::Disassembly => {
                if self.show_instruction_hex {
                    // Navigate left in hex bytes
                    if self.disasm_cursor_byte > 0 {
                        self.disasm_cursor_byte -= 1;
                    }
                }
            }
            FocusedPane::Memory => {
                // Move left by one column within the row
                if self.memory_cursor_col > 0 {
                    self.memory_cursor_col -= 1;
                } else if self.memory_scroll > 0 || self.memory_base_addr > 0 {
                    // Wrap to end of previous row
                    self.memory_cursor_col = MEMORY_NAV_COLS - 1;
                    if self.memory_scroll > 0 {
                        self.memory_scroll -= 1;
                    } else if self.memory_base_addr >= MEMORY_NAV_COLS {
                        self.memory_base_addr -= MEMORY_NAV_COLS;
                    }
                }
            }
            _ => {}
        }
    }

    pub(crate) fn navigate_right(&mut self, vm: &VM) {
        match self.focused_pane {
            FocusedPane::Disassembly => {
                if self.show_instruction_hex {
                    // Navigate right in hex bytes
                    if self.disasm_cursor_byte < 7 {
                        self.disasm_cursor_byte += 1;
                    }
                }
            }
            FocusedPane::Memory => {
                // Move right by one column within the row
                let current_addr = self.memory_base_addr
                    + self.memory_scroll * MEMORY_NAV_COLS
                    + self.memory_cursor_col;
                if self.memory_cursor_col < MEMORY_NAV_COLS - 1 {
                    if current_addr + 1 < vm.memory.len() {
                        self.memory_cursor_col += 1;
                    }
                } else {
                    // Wrap to beginning of next row
                    let next_row_addr =
                        self.memory_base_addr + (self.memory_scroll + 1) * MEMORY_NAV_COLS;
                    if next_row_addr < vm.memory.len() {
                        self.memory_cursor_col = 0;
                        self.memory_scroll += 1;
                        // Check if we need to scroll the view
                        let visible_rows = 15; // Approximate visible rows
                        if self.memory_scroll >= visible_rows {
                            self.memory_base_addr += MEMORY_NAV_COLS;
                            self.memory_scroll -= 1;
                        }
                    }
                }
            }
            _ => {}
        }
    }

    pub(crate) fn page_up(&mut self, _vm: &VM) {
        match self.focused_pane {
            FocusedPane::Disassembly => {
                self.disasm_scroll = self.disasm_scroll.saturating_sub(20);
            }
            FocusedPane::Memory => {
                // Page up in memory view
                let rows_to_move = 10;
                if self.memory_base_addr >= rows_to_move * MEMORY_NAV_COLS {
                    self.memory_base_addr -= rows_to_move * MEMORY_NAV_COLS;
                } else {
                    self.memory_base_addr = 0;
                    self.memory_scroll = 0;
                }
            }
            FocusedPane::Stack => {
                self.stack_scroll = self.stack_scroll.saturating_sub(10);
            }
            _ => {}
        }
    }

    pub(crate) fn page_down(&mut self, vm: &VM) {
        match self.focused_pane {
            FocusedPane::Disassembly => {
                self.disasm_scroll =
                    (self.disasm_scroll + 20).min(vm.instructions.len().saturating_sub(1));
            }
            FocusedPane::Memory => {
                // Page down in memory view
                let rows_to_move = 10;
                let new_base = self.memory_base_addr + rows_to_move * MEMORY_NAV_COLS;
                if new_base < vm.memory.len() {
                    self.memory_base_addr = new_base;
                }
            }
            FocusedPane::Stack => {
                let new_scroll = self.stack_scroll + 10;
                if new_scroll < self.execution_history.len() {
                    self.stack_scroll = new_scroll;
                } else if !self.execution_history.is_empty() {
                    self.stack_scroll = self.execution_history.len() - 1;
                }
            }
            _ => {}
        }
    }
}
