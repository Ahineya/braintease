use crate::settings::DebuggerSettings;
use crate::vm::{VMState, VM};
use crossterm::{
    event::{
        self, DisableMouseCapture, EnableMouseCapture, Event, MouseButton, MouseEvent,
        MouseEventKind,
    },
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use ratatui::{
    backend::{Backend, CrosstermBackend},
    layout::{Constraint, Direction, Layout, Rect},
    Frame, Terminal,
};
use ripple_asm::Register;
use std::collections::HashMap;
use std::io;
use std::time::{Duration, Instant};

// Fixed memory columns for navigation (actual display adjusts dynamically)
pub(crate) const MEMORY_NAV_COLS: usize = 8;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum FocusedPane {
    Disassembly,
    Registers,
    Memory,
    Stack,
    CallStack,
    Watches,
    Breakpoints,
    Output,
    #[allow(dead_code)]
    Command,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) enum DebuggerMode {
    Normal,
    Command,
    MemoryEdit,
    GotoAddress,
    AddWatch,
    SetBreakpoint,
}

pub struct MemoryWatch {
    pub(crate) name: String,
    pub(crate) address: usize,
    #[allow(dead_code)]
    pub(crate) size: usize,
    pub(crate) format: WatchFormat,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) enum WatchFormat {
    Hex,
    Decimal,
    Char,
    Binary,
}

pub struct TuiDebugger {
    // UI State
    pub(crate) focused_pane: FocusedPane,
    pub(crate) mode: DebuggerMode,

    // Scrolling positions
    pub(crate) disasm_scroll: usize,
    pub(crate) disasm_cursor_row: usize, // Row within visible area
    pub(crate) disasm_cursor_byte: usize, // Which byte in the instruction (0-7)
    pub(crate) memory_scroll: usize,
    pub(crate) memory_base_addr: usize,
    pub(crate) memory_cursor_col: usize, // Column position within the row (0-7)
    pub(crate) stack_scroll: usize,
    pub(crate) callstack_selected: usize, // Selected entry in call stack
    pub(crate) callstack_scroll: usize,
    pub(crate) output_scroll: usize,
    pub(crate) watches_scroll: usize,
    pub(crate) breakpoints_scroll: usize,

    // Debugging state
    pub(crate) breakpoints: HashMap<usize, bool>, // address -> enabled
    pub(crate) selected_breakpoint: usize,        // index in sorted breakpoints list
    pub(crate) memory_watches: Vec<MemoryWatch>,
    pub(crate) selected_watch: usize,

    // Command input
    pub(crate) command_buffer: String,
    pub(crate) command_history: Vec<String>,
    pub(crate) command_history_idx: usize,

    // Display preferences
    pub(crate) show_help: bool,
    pub(crate) help_scroll: usize,
    pub(crate) show_ascii: bool,
    pub(crate) show_instruction_hex: bool,
    pub(crate) show_debug_symbols: bool,

    // Panel visibility toggles
    pub(crate) show_registers: bool,
    pub(crate) show_memory: bool,
    pub(crate) show_stack: bool,
    pub(crate) show_callstack: bool,
    pub(crate) show_watches: bool,
    pub(crate) show_breakpoints: bool,
    pub(crate) show_output: bool,

    // Performance
    #[allow(dead_code)]
    last_step_time: Instant,
    #[allow(dead_code)]
    step_frequency: Duration,

    // Execution history
    pub(crate) execution_history: Vec<usize>,
    max_history: usize,

    // Register highlights (changed registers)
    pub(crate) register_changes: HashMap<usize, u16>,

    // Panel areas for mouse support
    panel_areas: HashMap<FocusedPane, Rect>,
    last_click_time: Option<Instant>,
    last_click_pos: Option<(u16, u16)>,

    // Status message for auto-pause and other notifications
    pub(crate) status_message: Option<String>,
}

impl Default for TuiDebugger {
    fn default() -> Self {
        Self::new()
    }
}

impl TuiDebugger {
    pub fn new() -> Self {
        // Load settings from disk
        let settings = DebuggerSettings::load();

        Self {
            focused_pane: FocusedPane::Disassembly,
            mode: DebuggerMode::Normal,

            disasm_scroll: 0,
            disasm_cursor_row: 0,
            disasm_cursor_byte: 0,
            memory_scroll: 0,
            memory_base_addr: 0,
            memory_cursor_col: 0,
            stack_scroll: 0,
            callstack_selected: 0,
            callstack_scroll: 0,
            output_scroll: 0,
            watches_scroll: 0,
            breakpoints_scroll: 0,

            breakpoints: HashMap::new(),
            selected_breakpoint: 0,
            memory_watches: Vec::new(),
            selected_watch: 0,

            command_buffer: String::new(),
            command_history: Vec::new(),
            command_history_idx: 0,

            show_help: false,
            help_scroll: 0,
            show_ascii: settings.show_ascii,
            show_instruction_hex: settings.show_instruction_hex,
            show_debug_symbols: true, // Enable by default

            show_registers: settings.show_registers,
            show_memory: settings.show_memory,
            show_stack: settings.show_stack,
            show_callstack: false, // New feature, disabled by default
            show_watches: settings.show_watches,
            show_breakpoints: settings.show_breakpoints,
            show_output: settings.show_output,

            last_step_time: Instant::now(),
            step_frequency: Duration::from_millis(100),

            execution_history: Vec::with_capacity(1000),
            max_history: 1000,

            register_changes: HashMap::new(),

            panel_areas: HashMap::new(),
            last_click_time: None,
            last_click_pos: None,
            status_message: None,
        }
    }

    pub fn run(&mut self, vm: &mut VM) -> io::Result<()> {
        // Setup terminal
        enable_raw_mode()?;
        let mut stdout = io::stdout();
        execute!(stdout, EnterAlternateScreen, EnableMouseCapture)?;
        let backend = CrosstermBackend::new(stdout);
        let mut terminal = Terminal::new(backend)?;

        // Clear screen
        terminal.clear()?;

        // Main loop
        let result = self.run_app(&mut terminal, vm);

        // Save settings before exiting
        self.save_settings();

        // Restore terminal
        disable_raw_mode()?;
        execute!(
            terminal.backend_mut(),
            LeaveAlternateScreen,
            DisableMouseCapture
        )?;
        terminal.show_cursor()?;

        result
    }

    fn save_settings(&self) {
        let settings = DebuggerSettings {
            show_registers: self.show_registers,
            show_memory: self.show_memory,
            show_stack: self.show_stack,
            show_watches: self.show_watches,
            show_breakpoints: self.show_breakpoints,
            show_output: self.show_output,
            show_ascii: self.show_ascii,
            show_instruction_hex: self.show_instruction_hex,
        };

        if let Err(e) = settings.save() {
            eprintln!("Warning: Failed to save debugger settings: {e}");
        }
    }

    fn run_app<B: Backend>(&mut self, terminal: &mut Terminal<B>, vm: &mut VM) -> io::Result<()> {
        'main: loop {
            // Draw UI
            terminal.draw(|f| self.draw_ui(f, vm))?;

            // Handle input
            if event::poll(Duration::from_millis(10))? {
                match event::read()? {
                    Event::Key(key) => {
                        match self.mode {
                            DebuggerMode::Normal => {
                                if !self.handle_normal_mode(key.code, key.modifiers, vm) {
                                    break 'main; // Exit requested
                                }
                            }
                            DebuggerMode::Command => {
                                if !self.handle_command_mode(key.code, vm) {
                                    break 'main; // Exit the debugger_ui
                                }
                            }
                            DebuggerMode::GotoAddress => {
                                self.handle_goto_mode(key.code);
                            }
                            DebuggerMode::AddWatch => {
                                self.handle_add_watch_mode(key.code);
                            }
                            DebuggerMode::SetBreakpoint => {
                                self.handle_breakpoint_mode(key.code, vm);
                            }
                            DebuggerMode::MemoryEdit => {
                                self.handle_memory_edit_mode(key.code, vm);
                            }
                        }
                    }
                    Event::Mouse(mouse) => {
                        self.handle_mouse_event(mouse, vm);
                    }
                    _ => {}
                }
            }
        }

        Ok(())
    }

    fn draw_ui(&mut self, frame: &mut Frame, vm: &VM) {
        let size = frame.size();

        // Clear panel areas for fresh tracking
        self.panel_areas.clear();

        // Reserve bottom line for status/input
        let main_area = Rect::new(0, 0, size.width, size.height - 1);
        let status_area = Rect::new(0, size.height - 1, size.width, 1);

        // Main layout: 2 columns - left for disassembly/output, right for registers/memory/stack/watches
        let main_chunks = Layout::default()
            .direction(Direction::Horizontal)
            .constraints([
                Constraint::Percentage(45), // Left: Disassembly + Output
                Constraint::Percentage(55), // Right: Registers + Memory + Stack/Watches
            ])
            .split(main_area);

        // Left column: Build constraints based on visible panels
        let mut left_constraints = vec![];
        if self.show_output {
            left_constraints.push(Constraint::Min(15)); // Disassembly
            left_constraints.push(Constraint::Length(10)); // Output
        } else {
            left_constraints.push(Constraint::Min(15)); // Disassembly takes full space
        }

        let left_chunks = Layout::default()
            .direction(Direction::Vertical)
            .constraints(left_constraints)
            .split(main_chunks[0]);

        // Right column: Split horizontally only if we have panels on both sides
        let show_left_panels = self.show_registers || self.show_memory;
        let show_right_panels =
            self.show_stack || self.show_callstack || self.show_watches || self.show_breakpoints;

        let (left_panel_area, right_panel_area) = if show_left_panels && show_right_panels {
            let right_chunks = Layout::default()
                .direction(Direction::Horizontal)
                .constraints([
                    Constraint::Percentage(65), // Left: Registers + Memory
                    Constraint::Percentage(35), // Right: Stack + Watches
                ])
                .split(main_chunks[1]);
            (Some(right_chunks[0]), Some(right_chunks[1]))
        } else if show_left_panels {
            (Some(main_chunks[1]), None)
        } else if show_right_panels {
            (None, Some(main_chunks[1]))
        } else {
            (None, None)
        };

        // Left part of right side: Registers and Memory
        if let Some(area) = left_panel_area {
            let mut constraints = vec![];
            if self.show_registers && self.show_memory {
                constraints.push(Constraint::Length(11)); // Registers
                constraints.push(Constraint::Min(14)); // Memory
            } else if self.show_registers {
                constraints.push(Constraint::Min(11)); // Registers only
            } else if self.show_memory {
                constraints.push(Constraint::Min(14)); // Memory only
            }

            if !constraints.is_empty() {
                let chunks = Layout::default()
                    .direction(Direction::Vertical)
                    .constraints(constraints)
                    .split(area);

                let mut chunk_idx = 0;
                if self.show_registers {
                    self.panel_areas
                        .insert(FocusedPane::Registers, chunks[chunk_idx]);
                    self.draw_registers(frame, chunks[chunk_idx], vm);
                    chunk_idx += 1;
                }
                if self.show_memory {
                    self.panel_areas
                        .insert(FocusedPane::Memory, chunks[chunk_idx]);
                    self.draw_memory(frame, chunks[chunk_idx], vm);
                }
            }
        }

        // Right part: Stack, CallStack, Watches, and Breakpoints
        if let Some(area) = right_panel_area {
            let mut constraints = vec![];
            let mut panels = vec![];

            if self.show_stack {
                constraints.push(Constraint::Ratio(1, 3));
                panels.push("stack");
            }
            if self.show_callstack {
                constraints.push(Constraint::Ratio(1, 3));
                panels.push("callstack");
            }
            if self.show_watches {
                constraints.push(Constraint::Ratio(1, 3));
                panels.push("watches");
            }
            if self.show_breakpoints {
                constraints.push(Constraint::Ratio(1, 3));
                panels.push("breakpoints");
            }

            // Adjust constraints based on number of visible panels
            let visible_count = panels.len() as u32;
            if visible_count > 0 {
                constraints.clear();
                for _ in 0..visible_count {
                    constraints.push(Constraint::Ratio(1, visible_count));
                }

                let chunks = Layout::default()
                    .direction(Direction::Vertical)
                    .constraints(constraints)
                    .split(area);

                let mut chunk_idx = 0;
                for panel in panels {
                    match panel {
                        "stack" => {
                            self.panel_areas
                                .insert(FocusedPane::Stack, chunks[chunk_idx]);
                            self.draw_stack(frame, chunks[chunk_idx], vm);
                            chunk_idx += 1;
                        }
                        "callstack" => {
                            self.panel_areas
                                .insert(FocusedPane::CallStack, chunks[chunk_idx]);
                            self.draw_callstack(frame, chunks[chunk_idx], vm);
                            chunk_idx += 1;
                        }
                        "watches" => {
                            self.panel_areas
                                .insert(FocusedPane::Watches, chunks[chunk_idx]);
                            self.draw_watches(frame, chunks[chunk_idx], vm);
                            chunk_idx += 1;
                        }
                        "breakpoints" => {
                            self.panel_areas
                                .insert(FocusedPane::Breakpoints, chunks[chunk_idx]);
                            self.draw_breakpoints(frame, chunks[chunk_idx], vm);
                            chunk_idx += 1;
                        }
                        _ => {}
                    }
                }
            }
        }

        // Always draw disassembly
        self.panel_areas
            .insert(FocusedPane::Disassembly, left_chunks[0]);
        self.draw_disassembly(frame, left_chunks[0], vm);

        // Draw output if visible
        if self.show_output && left_chunks.len() > 1 {
            self.panel_areas.insert(FocusedPane::Output, left_chunks[1]);
            self.draw_output(frame, left_chunks[1], vm);
        }

        // Draw status/input line at bottom
        match self.mode {
            DebuggerMode::Command => {
                if self.command_buffer == "toggle:" {
                    self.draw_input_line(frame, status_area, "Toggle Panel (2-8, 1=Disasm fixed)");
                } else {
                    self.draw_input_line(frame, status_area, "Command");
                }
            }
            DebuggerMode::GotoAddress => {
                self.draw_input_line(frame, status_area, "Go to address (hex)")
            }
            DebuggerMode::AddWatch => {
                self.draw_input_line(frame, status_area, "Add watch (name:addr[:format])")
            }
            DebuggerMode::SetBreakpoint => {
                self.draw_input_line(frame, status_area, "Set breakpoint.rs (instr# or 0xAddr)")
            }
            DebuggerMode::MemoryEdit => {
                // Try to show current value at address
                let mut prompt = String::from("Edit memory (addr:value)");
                if self.command_buffer.contains(':') {
                    if let Some(colon_pos) = self.command_buffer.find(':') {
                        let addr_str = &self.command_buffer[..colon_pos];
                        if let Ok(addr) =
                            usize::from_str_radix(addr_str.trim_start_matches("0x"), 16)
                        {
                            if addr < vm.memory.len() {
                                prompt = format!(
                                    "Edit memory @ 0x{:04X} (current: 0x{:04X})",
                                    addr, vm.memory[addr]
                                );
                            }
                        }
                    }
                }
                self.draw_input_line(frame, status_area, &prompt)
            }
            DebuggerMode::Normal => self.draw_status_line(frame, status_area, vm),
        }

        // Draw help overlay if enabled
        if self.show_help {
            self.draw_help(frame, size);
        }
    }

    pub(crate) fn step_vm(&mut self, vm: &mut VM) {
        // Check for breakpoint.rs BEFORE executing
        let pc = vm.registers[Register::Pc as usize] as usize;
        let pcb = vm.registers[Register::Pcb as usize] as usize;
        let addr = pcb * vm.bank_size as usize + pc;

        // Only stop at enabled breakpoints if we're in Running state (not already at a breakpoint)
        if let Some(&enabled) = self.breakpoints.get(&addr) {
            if enabled && matches!(vm.state, VMState::Running) {
                vm.state = VMState::Breakpoint;
                return;
            }
        }

        self.step_vm_no_break_check(vm);
    }

    pub(crate) fn step_vm_no_break_check(&mut self, vm: &mut VM) {
        // Save current registers for change detection
        let old_registers = vm.registers;

        // Get current PC for history
        let pc = vm.registers[Register::Pc as usize] as usize;
        let pcb = vm.registers[Register::Pcb as usize] as usize;
        let addr = pcb * vm.bank_size as usize + pc;

        // Record execution history
        self.execution_history.push(addr);
        if self.execution_history.len() > self.max_history {
            self.execution_history.remove(0);
        }

        // Step the VM
        let _ = vm.step();

        // Track register changes
        for i in 0..18 {
            if old_registers[i] != vm.registers[i] {
                self.register_changes.insert(i, old_registers[i]);
            }
        }

        // Auto-scroll disassembly to keep PC visible
        let new_pc = vm.registers[Register::Pc as usize] as usize;
        let new_pcb = vm.registers[Register::Pcb as usize] as usize;
        let new_addr = new_pcb * vm.bank_size as usize + new_pc;

        if new_addr < self.disasm_scroll || new_addr >= self.disasm_scroll + 20 {
            self.disasm_scroll = new_addr.saturating_sub(5);
        }
    }

    pub(crate) fn run_until_break(&mut self, vm: &mut VM) {
        const MAX_STEPS_BEFORE_PAUSE: usize = 10_000_000; // Pause after 10 million steps to prevent hanging
        let mut steps_executed = 0;

        while matches!(vm.state, VMState::Running) {
            // Step will check for breakpoint.rs before executing
            self.step_vm(vm);
            steps_executed += 1;

            // If we hit a breakpoint.rs or other state change, stop
            if !matches!(vm.state, VMState::Running) {
                break;
            }

            // Safety check: pause if we've executed too many steps (likely infinite loop)
            if steps_executed >= MAX_STEPS_BEFORE_PAUSE {
                // Set state to breakpoint to pause execution
                vm.state = VMState::Breakpoint;
                // Store status message to display in UI
                self.status_message = Some(format!(
                    "Auto-paused after {}M steps (possible infinite loop)",
                    MAX_STEPS_BEFORE_PAUSE / 1_000_000
                ));
                break;
            }
        }
    }

    fn handle_mouse_event(&mut self, mouse: MouseEvent, vm: &mut VM) {
        // Only handle left button clicks in normal mode
        if self.mode != DebuggerMode::Normal {
            return;
        }

        match mouse.kind {
            MouseEventKind::Down(MouseButton::Left) => {
                let click_pos = (mouse.column, mouse.row);
                let now = Instant::now();

                // Check for double-click (within 500ms and same position)
                let is_double_click = if let (Some(last_time), Some(last_pos)) =
                    (self.last_click_time, self.last_click_pos)
                {
                    now.duration_since(last_time).as_millis() < 500 && last_pos == click_pos
                } else {
                    false
                };

                // Update click tracking
                self.last_click_time = Some(now);
                self.last_click_pos = Some(click_pos);

                // Check which panel was clicked
                for (pane, rect) in &self.panel_areas {
                    if click_pos.0 >= rect.x
                        && click_pos.0 < rect.x + rect.width
                        && click_pos.1 >= rect.y
                        && click_pos.1 < rect.y + rect.height
                    {
                        // Found the clicked panel
                        self.focused_pane = *pane;

                        // Handle double-click actions
                        if is_double_click {
                            match pane {
                                FocusedPane::Disassembly => {
                                    // Double-click in disassembly toggles breakpoint at clicked line
                                    let relative_row = (click_pos.1 - rect.y) as usize;
                                    let addr = self.disasm_scroll + relative_row;
                                    if addr < vm.instructions.len() {
                                        if let std::collections::hash_map::Entry::Vacant(e) =
                                            self.breakpoints.entry(addr)
                                        {
                                            e.insert(true);
                                        } else {
                                            self.breakpoints.remove(&addr);
                                        }
                                    }
                                }
                                FocusedPane::Memory => {
                                    // Double-click in memory enters edit mode
                                    let relative_row = (click_pos.1 - rect.y) as usize;
                                    let relative_col = (click_pos.0 - rect.x) as usize;

                                    // Calculate which memory cell was clicked (rough estimate)
                                    // Each cell takes about 5 chars (4 hex + space)
                                    let col_offset = relative_col.saturating_sub(10) / 5; // Skip address prefix
                                    if col_offset < MEMORY_NAV_COLS {
                                        let addr = self.memory_base_addr
                                            + relative_row * MEMORY_NAV_COLS
                                            + col_offset;
                                        self.command_buffer = format!("{addr:04x}:");
                                        self.mode = DebuggerMode::MemoryEdit;
                                    }
                                }
                                _ => {}
                            }
                        }
                        break;
                    }
                }
            }
            MouseEventKind::ScrollDown => {
                // Scroll down in the focused pane
                match self.focused_pane {
                    FocusedPane::Disassembly => {
                        self.disasm_scroll = self.disasm_scroll.saturating_add(3);
                    }
                    FocusedPane::Memory => {
                        self.memory_scroll = self.memory_scroll.saturating_add(1);
                    }
                    FocusedPane::Stack => {
                        self.stack_scroll = self.stack_scroll.saturating_add(1);
                    }
                    FocusedPane::CallStack => {
                        self.callstack_scroll = self.callstack_scroll.saturating_add(1);
                    }
                    FocusedPane::Output => {
                        self.output_scroll = self.output_scroll.saturating_add(1);
                    }
                    FocusedPane::Watches => {
                        if self.selected_watch < self.memory_watches.len().saturating_sub(1) {
                            self.selected_watch += 1;
                        }
                    }
                    FocusedPane::Breakpoints => {
                        if self.selected_breakpoint < self.breakpoints.len().saturating_sub(1) {
                            self.selected_breakpoint += 1;
                        }
                    }
                    _ => {}
                }
            }
            MouseEventKind::ScrollUp => {
                // Scroll up in the focused pane
                match self.focused_pane {
                    FocusedPane::Disassembly => {
                        self.disasm_scroll = self.disasm_scroll.saturating_sub(3);
                    }
                    FocusedPane::Memory => {
                        if self.memory_scroll > 0 {
                            self.memory_scroll -= 1;
                        } else if self.memory_base_addr >= MEMORY_NAV_COLS {
                            self.memory_base_addr -= MEMORY_NAV_COLS;
                        }
                    }
                    FocusedPane::Stack => {
                        self.stack_scroll = self.stack_scroll.saturating_sub(1);
                    }
                    FocusedPane::CallStack => {
                        self.callstack_scroll = self.callstack_scroll.saturating_sub(1);
                    }
                    FocusedPane::Output => {
                        self.output_scroll = self.output_scroll.saturating_sub(1);
                    }
                    FocusedPane::Watches => {
                        if self.selected_watch > 0 {
                            self.selected_watch -= 1;
                        }
                    }
                    FocusedPane::Breakpoints => {
                        if self.selected_breakpoint > 0 {
                            self.selected_breakpoint -= 1;
                        }
                    }
                    _ => {}
                }
            }
            _ => {}
        }
    }
}
