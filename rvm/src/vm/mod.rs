/// VM module - Virtual Machine implementation for the Ripple architecture

mod instruction;
mod state;
mod mmio;
mod display;
mod terminal;
mod execution;
mod storage;

pub use instruction::Instr;
pub use state::{VMState, KeyboardState};
pub use terminal::install_terminal_cleanup_hook;

use std::collections::{VecDeque, HashMap};
use ripple_asm::Register;
use crate::constants::*;
use crate::display_rgb565::RGB565Display;
use crate::vm::storage::Storage;

/// The Ripple Virtual Machine
pub struct VM {
    // Program memory - stores instructions
    pub instructions: Vec<Instr>,
    
    // Data memory - separate from instructions
    pub memory: Vec<u16>,
    
    // Registers (32 total: R0-R31)
    pub registers: [u16; 32],
    
    // VM state
    pub state: VMState,
    
    // Configuration
    pub bank_size: u16,
    
    // Debug mode flag
    pub debug_mode: bool,
    
    // Verbose mode flag
    pub verbose: bool,
    
    // Do not increment PC flag (set by jump/branch instructions)
    skip_pc_increment: bool,
    
    // Output buffer for I/O
    pub output_buffer: VecDeque<u8>,
    output_ready: bool,
    stdout_dirty: bool,
    
    // Input buffer for TTY_IN
    pub input_buffer: VecDeque<u8>,
    
    // TTY input mode state
    tty_input_enabled: bool,
    
    // RNG state (simple LCG)
    rng_state: u32,
    
    // Display state
    display_mode: u16,
    display_enabled: bool,
    display_flush_done: bool,
    
    // Terminal state for TEXT40 mode
    terminal_raw_mode: bool,
    terminal_saved_screen: bool,
    
    // Keyboard state (for TEXT40 mode)
    keyboard_state: KeyboardState,
    
    // RGB565 display
    pub rgb565_display: Option<RGB565Display>,
    pub display_resolution: u16, // hi8=width, lo8=height
    
    // Debug information: maps instruction indices to function names
    pub debug_symbols: HashMap<usize, String>,
    
    // Storage subsystem
    storage: Option<Storage>,

    // Interrupt controller
    irq_status: u16,
    irq_enable: u16,
    irq_busy: u16,
    irq_vector_bank: u16,
    irq_vector_off: u16,
    irq_cause: u16,
}

impl VM {
    pub fn new(bank_size: u16) -> Self {
        Self::with_memory_size(bank_size, DEFAULT_MEMORY_SIZE)
    }
    
    pub fn with_memory_size(bank_size: u16, memory_size: usize) -> Self {
        Self::with_options(bank_size, memory_size, None)
    }
    
    pub fn with_options(bank_size: u16, memory_size: usize, disk_path: Option<std::path::PathBuf>) -> Self {
        let memory_size = memory_size.max(MIN_MEMORY_SIZE);
        
        // Try to initialize storage, but don't fail if it can't be created
        let storage = match disk_path {
            Some(path) => match Storage::with_path(path) {
                Ok(s) => Some(s),
                Err(e) => {
                    eprintln!("Warning: Could not initialize storage with custom path: {}", e);
                    None
                }
            },
            None => match Storage::new() {
                Ok(s) => Some(s),
                Err(e) => {
                    eprintln!("Warning: Could not initialize storage: {}", e);
                    None
                }
            }
        };
        
        VM {
            instructions: Vec::new(),
            memory: vec![0; memory_size],
            registers: [0; 32],
            state: VMState::Setup,
            bank_size,
            debug_mode: false,
            verbose: false,
            skip_pc_increment: false,
            output_buffer: VecDeque::new(),
            output_ready: true,
            stdout_dirty: false,
            input_buffer: VecDeque::new(),
            tty_input_enabled: false,
            rng_state: 0x12345678,  // Fixed seed for reproducibility
            display_mode: DISP_OFF,
            display_enabled: false,
            display_flush_done: true,
            terminal_raw_mode: false,
            terminal_saved_screen: false,
            keyboard_state: KeyboardState::default(),
            rgb565_display: None,
            display_resolution: 0,
            debug_symbols: HashMap::new(),
            storage,
            irq_status: 0,
            irq_enable: 0,
            irq_busy: 0,
            irq_vector_bank: 0,
            irq_vector_off: 0,
            irq_cause: 0,
        }
    }
    
    #[allow(dead_code)]
    pub fn new_default() -> Self {
        Self::new(DEFAULT_BANK_SIZE)
    }
    
    pub fn set_rng_seed(&mut self, seed: u32) {
        self.rng_state = seed;
    }
    
    pub fn load_binary(&mut self, binary: &[u8]) -> Result<(), String> {
        // Check magic number
        if binary.len() < 5 || &binary[0..5] != MAGIC_RLINK {
            return Err("Invalid binary format: missing RLINK magic".to_string());
        }
        
        let mut pos = 5;
        
        // Read bank size (new field in binary format)
        // Always expect it to be present in new format
        if pos + 2 > binary.len() {
            return Err("Invalid binary: missing bank size".to_string());
        }
        let binary_bank_size = u16::from_le_bytes([binary[pos], binary[pos+1]]);
        if binary_bank_size != 0 {  // 0 means not specified, use default
            self.bank_size = binary_bank_size;
        }
        pos += 2;
        
        // Read entry point
        if pos + 4 > binary.len() {
            return Err("Invalid binary: missing entry point".to_string());
        }
        let entry_point = u32::from_le_bytes([
            binary[pos], binary[pos+1], binary[pos+2], binary[pos+3]
        ]);
        pos += 4;
        
        // Read instruction count
        if pos + 4 > binary.len() {
            return Err("Invalid binary: missing instruction count".to_string());
        }
        let instruction_count = u32::from_le_bytes([
            binary[pos], binary[pos+1], binary[pos+2], binary[pos+3]
        ]) as usize;
        pos += 4;
        
        // Code may span multiple banks. Fetch is PCB * bank_size + PC.
        // rlink NOP-pads so bank N starts at index N * bank_size.
        
        // Read instructions
        self.instructions.clear();
        for i in 0..instruction_count {
            if pos + INSTRUCTION_SIZE > binary.len() {
                return Err(format!("Invalid binary: missing instruction {i}"));
            }
            
            let instr = Instr {
                opcode: binary[pos],
                word0: binary[pos + 1],
                word1: u16::from_le_bytes([binary[pos + 2], binary[pos + 3]]),
                word2: u16::from_le_bytes([binary[pos + 4], binary[pos + 5]]),
                word3: u16::from_le_bytes([binary[pos + 6], binary[pos + 7]]),
            };
            self.instructions.push(instr);
            pos += INSTRUCTION_SIZE;
        }
        
        // Read data section size
        if pos + 4 > binary.len() {
            return Err("Invalid binary: missing data size".to_string());
        }
        let data_size = u32::from_le_bytes([
            binary[pos], binary[pos+1], binary[pos+2], binary[pos+3]
        ]) as usize;
        pos += 4;
        
        // Read data section
        if pos + data_size > binary.len() {
            return Err("Invalid binary: incomplete data section".to_string());
        }
        
        // Determine data section offset based on display mode
        // Note: At this point, display mode might not be set yet, so we need to check
        // if the binary is intended for RGB565 mode by looking for display setup
        let data_offset = self.get_data_section_offset();
        
        // Load data into memory starting at the calculated offset
        for (i, &byte) in binary[pos..pos + data_size].iter().enumerate() {
            if i + data_offset < self.memory.len() {
                self.memory[i + data_offset] = byte as u16;
            }
        }
        pos += data_size;
        
        // Try to read debug section if present
        if pos + 5 <= binary.len() && &binary[pos..pos + 5] == b"DEBUG" {
            pos += 5;
            
            // Read number of debug entries
            if pos + 4 <= binary.len() {
                let debug_count = u32::from_le_bytes([
                    binary[pos], binary[pos+1], binary[pos+2], binary[pos+3]
                ]) as usize;
                pos += 4;
                
                // Read each debug entry
                for _ in 0..debug_count {
                    if pos + 4 > binary.len() {
                        break; // Incomplete debug info, just skip
                    }
                    
                    // Read name length
                    let name_len = u32::from_le_bytes([
                        binary[pos], binary[pos+1], binary[pos+2], binary[pos+3]
                    ]) as usize;
                    pos += 4;
                    
                    if pos + name_len > binary.len() {
                        break; // Incomplete debug info
                    }
                    
                    // Read name
                    let name = String::from_utf8_lossy(&binary[pos..pos + name_len]).to_string();
                    pos += name_len;
                    
                    if pos + 4 > binary.len() {
                        break; // Incomplete debug info
                    }
                    
                    // Read instruction index
                    let instr_idx = u32::from_le_bytes([
                        binary[pos], binary[pos+1], binary[pos+2], binary[pos+3]
                    ]) as usize;
                    pos += 4;
                    
                    // Store in debug symbols map
                    self.debug_symbols.insert(instr_idx, name);
                }
            }
        }
        
        // Set entry point
        let entry_bank = (entry_point / (self.bank_size as u32 * 4)) as u16;
        let entry_offset = ((entry_point / 4) % (self.bank_size as u32)) as u16;
        self.registers[Register::Pcb as usize] = entry_bank;
        self.registers[Register::Pc as usize] = entry_offset;
        
        // Initialize MMIO state (memory is handled via MMIO read/write handlers)
        self.output_ready = true;
        self.display_mode = DISP_OFF;
        self.display_enabled = false;
        self.display_flush_done = true;
        
        self.state = VMState::Running;
        Ok(())
    }
    
    pub fn step(&mut self) -> Result<(), String> {
        match self.state {
            VMState::Running => {},
            VMState::Halted => return Ok(()),
            VMState::Breakpoint => {
                // In debug mode at breakpoint, allow single stepping
                // State will be reset to Running by the debugger_ui
            },
            VMState::Error(ref e) => return Err(e.clone()),
            VMState::Setup => return Err("VM not initialized".to_string()),
        }
        
        // Keyboard/TTY input is polled from MMIO reads (TTY_IN_STATUS / KEY_*),
        // not every instruction — crossterm event::poll is a syscall.

        self.maybe_dispatch_irq();

        // Calculate instruction address
        let pc = self.registers[Register::Pc as usize];
        let pcb = self.registers[Register::Pcb as usize];
        let instr_idx = (pcb as usize * self.bank_size as usize) + pc as usize;
        
        if instr_idx >= self.instructions.len() {
            self.state = VMState::Error(format!("PC out of bounds: bank={}, offset={}, idx={}, total_instructions={}", 
                                               pcb, pc, instr_idx, self.instructions.len()));
            return Err(format!("PC out of bounds: idx={} >= len={}", instr_idx, self.instructions.len()));
        }
        
        let instr = self.instructions[instr_idx];
        self.skip_pc_increment = false;
        
        // Print instruction in verbose mode
        if self.verbose {
            eprint!("[{instr_idx:04X}] ");
            self.print_instruction(&instr);
        }
        
        // Execute instruction
        self.execute_instruction(instr)?;
        
        // Increment PC unless instruction set the skip flag
        if !self.skip_pc_increment {
            let mut new_pc = self.registers[Register::Pc as usize] as u32 + 1;
            let mut new_pcb = self.registers[Register::Pcb as usize] as u32;
            
            if new_pc >= self.bank_size as u32 {
                new_pc = 0;
                new_pcb += 1;
            }
            
            self.registers[Register::Pc as usize] = (new_pc & 0xFFFF) as u16;
            self.registers[Register::Pcb as usize] = (new_pcb & 0xFFFF) as u16;
        }
        
        Ok(())
    }
    
    pub fn run(&mut self) -> Result<(), String> {
        while matches!(self.state, VMState::Running) {
            self.step()?;
            // Stop if we hit a breakpoint in debug mode
            if matches!(self.state, VMState::Breakpoint) {
                break;
            }
        }
        Ok(())
    }

    /// If a pending IRQ is enabled and none is in service, jump to the vector.
    /// Saves PC/PCB into RA/RAB like JAL so the handler can RET.
    fn maybe_dispatch_irq(&mut self) {
        if self.irq_busy != 0 {
            return;
        }
        let ready = self.irq_status & self.irq_enable;
        if ready == 0 {
            return;
        }
        let bit = ready.trailing_zeros() as u16;
        self.irq_busy = 1;
        self.irq_cause = bit + 1;
        self.sync_irq_memory();

        self.registers[Register::Ra as usize] = self.registers[Register::Pc as usize];
        self.registers[Register::Rab as usize] = self.registers[Register::Pcb as usize];
        self.registers[Register::Pcb as usize] = self.irq_vector_bank;
        self.registers[Register::Pc as usize] = self.irq_vector_off;
    }

    fn sync_irq_memory(&mut self) {
        if self.memory.len() > HDR_IRQ_CAUSE {
            self.memory[HDR_IRQ_STATUS] = self.irq_status;
            self.memory[HDR_IRQ_ENABLE] = self.irq_enable;
            self.memory[HDR_IRQ_BUSY] = self.irq_busy;
            self.memory[HDR_IRQ_VECTOR_BANK] = self.irq_vector_bank;
            self.memory[HDR_IRQ_VECTOR_OFF] = self.irq_vector_off;
            self.memory[HDR_IRQ_CAUSE] = self.irq_cause;
        }
    }

    pub fn get_output(&mut self) -> Vec<u8> {
        self.output_buffer.drain(..).collect()
    }
    
    #[allow(dead_code)]
    pub fn push_input(&mut self, byte: u8) {
        self.input_buffer.push_back(byte);
    }
    
    #[allow(dead_code)]
    pub fn push_input_string(&mut self, input: &str) {
        for byte in input.bytes() {
            self.input_buffer.push_back(byte);
        }
    }
    
    pub fn get_current_instruction(&self) -> Option<Instr> {
        let pc = self.registers[Register::Pc as usize];
        let pcb = self.registers[Register::Pcb as usize];
        let idx = (pcb as usize * self.bank_size as usize) + pc as usize;
        
        if idx < self.instructions.len() {
            Some(self.instructions[idx])
        } else {
            None
        }
    }
    
    pub fn reset(&mut self) {
        // Clear registers but preserve bank size
        self.registers = [0; 32];
        
        // Reset to entry point (instruction 0)
        self.registers[Register::Pc as usize] = 0;
        self.registers[Register::Pcb as usize] = 0;
        
        // Reset state to running (ready to execute)
        self.state = VMState::Running;
        self.skip_pc_increment = false;
        
        // Clear I/O buffers
        self.output_buffer.clear();
        self.input_buffer.clear();
        self.output_ready = true;
        self.stdout_dirty = false;
        
        // Disable TTY input if it was enabled
        if self.tty_input_enabled {
            self.disable_tty_input();
        }
        
        // Clear keyboard state
        self.clear_keyboard_state();
        
        // Reset display state (and exit any active display mode)
        if self.display_mode == DISP_TTY && self.terminal_raw_mode {
            self.exit_tty_mode();
        } else if self.display_mode == DISP_TEXT40 && self.terminal_raw_mode {
            self.exit_text40_mode();
        } else if self.display_mode == DISP_RGB565 {
            if let Some(mut display) = self.rgb565_display.take() {
                display.shutdown();
            }
        }
        self.display_mode = DISP_OFF;
        self.display_enabled = false;
        self.display_flush_done = true;
        self.display_resolution = 0;

        self.irq_status = 0;
        self.irq_enable = 0;
        self.irq_busy = 0;
        self.irq_vector_bank = 0;
        self.irq_vector_off = 0;
        self.irq_cause = 0;
        
        // Clear all memory (reset to zeros)
        self.memory.fill(0);
        
        // Note: We keep the loaded instructions, data, debug symbols, and storage intact
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::constants::MAGIC_RLINK;
    use ripple_asm::Register;

    fn instr(opcode: u8, word1: u16, word2: u16, word3: u16) -> Instr {
        Instr {
            opcode,
            word0: opcode,
            word1,
            word2,
            word3,
        }
    }

    fn nop() -> Instr {
        instr(0x00, 1, 0, 0)
    }

    fn halt() -> Instr {
        instr(0x00, 0, 0, 0)
    }

    #[test]
    fn jal_saves_return_bank_then_jumps() {
        let mut vm = VM::with_memory_size(8, 256);
        let mut instructions = vec![
            instr(0x13, Register::Ra as u16, 1, 0), // JAL RA, bank 1, addr 0
            halt(),
        ];
        while instructions.len() < 8 {
            instructions.push(nop());
        }
        instructions.push(instr(0x0E, Register::Rv0 as u16, 42, 0)); // LI RV0, 42
        instructions.push(instr(0x14, 0, Register::Rab as u16, Register::Ra as u16)); // RET
        vm.instructions = instructions;
        vm.state = VMState::Running;

        vm.step().unwrap();
        assert_eq!(vm.registers[Register::Pcb as usize], 1);
        assert_eq!(vm.registers[Register::Pc as usize], 0);
        assert_eq!(vm.registers[Register::Ra as usize], 1);
        assert_eq!(vm.registers[Register::Rab as usize], 0);

        vm.step().unwrap();
        assert_eq!(vm.registers[Register::Rv0 as usize], 42);

        vm.step().unwrap();
        assert_eq!(vm.registers[Register::Pcb as usize], 0);
        assert_eq!(vm.registers[Register::Pc as usize], 1);

        vm.step().unwrap();
        assert!(matches!(vm.state, VMState::Halted));
    }

    #[test]
    fn load_binary_accepts_multi_bank_image() {
        let mut binary = Vec::new();
        binary.extend_from_slice(MAGIC_RLINK);
        binary.extend_from_slice(&8u16.to_le_bytes()); // bank size
        binary.extend_from_slice(&0u32.to_le_bytes()); // entry point
        binary.extend_from_slice(&10u32.to_le_bytes()); // 10 instructions > bank size 8
        for i in 0..10 {
            let inst = if i == 0 { halt() } else { nop() };
            binary.push(inst.opcode);
            binary.push(inst.word0);
            binary.extend_from_slice(&inst.word1.to_le_bytes());
            binary.extend_from_slice(&inst.word2.to_le_bytes());
            binary.extend_from_slice(&inst.word3.to_le_bytes());
        }
        binary.extend_from_slice(&0u32.to_le_bytes()); // data size

        let mut vm = VM::with_memory_size(8, 256);
        vm.load_binary(&binary).expect("multi-bank image should load");
        assert_eq!(vm.instructions.len(), 10);
        assert_eq!(vm.bank_size, 8);
    }

    #[test]
    fn loadc_reads_instruction_memory_not_data() {
        let mut vm = VM::with_memory_size(8, 256);
        // idx 0: LOADC R0, R0, T0
        // idx 1: HALT
        // idx 2: ADDI T7, T6, 0x42  (payload)
        vm.instructions = vec![
            instr(0x20, Register::R0 as u16, Register::R0 as u16, Register::T0 as u16),
            halt(),
            instr(0x0A, Register::T7 as u16, Register::T6 as u16, 0x42),
            nop(), nop(), nop(), nop(), nop(),
        ];
        vm.memory[2] = 0xDEAD;
        vm.registers[Register::T0 as usize] = 2;
        vm.state = VMState::Running;

        vm.step().unwrap();
        assert_eq!(vm.registers[Register::X0 as usize], 0x0A);
        assert_eq!(vm.registers[Register::X1 as usize], Register::T7 as u16);
        assert_eq!(vm.registers[Register::X2 as usize], Register::T6 as u16);
        assert_eq!(vm.registers[Register::X3 as usize], 0x42);
        assert_eq!(vm.memory[2], 0xDEAD);
    }

    #[test]
    fn storc_writes_instruction_memory_not_data() {
        let mut vm = VM::with_memory_size(8, 256);
        vm.instructions = vec![
            instr(0x21, Register::R0 as u16, Register::R0 as u16, Register::T0 as u16),
            halt(),
            nop(),
            nop(), nop(), nop(), nop(), nop(),
        ];
        vm.memory[2] = 0xBEEF;
        vm.registers[Register::T0 as usize] = 2;
        vm.registers[Register::X0 as usize] = 0x0E;
        vm.registers[Register::X1 as usize] = Register::A0 as u16;
        vm.registers[Register::X2 as usize] = 65;
        vm.registers[Register::X3 as usize] = 0;
        vm.state = VMState::Running;

        vm.step().unwrap();
        let written = vm.instructions[2];
        assert_eq!(written.opcode, 0x0E);
        assert_eq!(written.word0, 0x0E);
        assert_eq!(written.word1, Register::A0 as u16);
        assert_eq!(written.word2, 65);
        assert_eq!(written.word3, 0);
        assert_eq!(vm.memory[2], 0xBEEF);
    }

    #[test]
    fn irq_registers_read_write_without_dispatch() {
        let mut vm = VM::with_memory_size(8, 256);
        vm.instructions = vec![halt(), nop(), nop(), nop(), nop(), nop(), nop(), nop()];
        vm.state = VMState::Running;

        assert!(vm.handle_mmio_write(HDR_IRQ_ENABLE, 0x0005));
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_ENABLE), Some(0x0005));
        assert!(vm.handle_mmio_write(HDR_IRQ_VECTOR_BANK, 3));
        assert!(vm.handle_mmio_write(HDR_IRQ_VECTOR_OFF, 0x1234));
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_VECTOR_BANK), Some(3));
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_VECTOR_OFF), Some(0x1234));
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_BUSY), Some(0));
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_CAUSE), Some(0));
        assert_eq!(vm.handle_mmio_read(27), Some(0));
        vm.handle_mmio_write(HDR_IRQ_CAUSE, 7);
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_CAUSE), Some(0));
        vm.handle_mmio_write(HDR_IRQ_BUSY, 1); // not in-service: ignore
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_BUSY), Some(0));

        // Pending without overlapping enable: no dispatch
        vm.handle_mmio_write(HDR_IRQ_ENABLE, 0);
        vm.handle_mmio_write(HDR_IRQ_STATUS, IRQ_SW);
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_STATUS), Some(IRQ_SW));
        vm.step().unwrap();
        assert!(matches!(vm.state, VMState::Halted));
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_BUSY), Some(0));
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_CAUSE), Some(0));
    }

    #[test]
    fn irq_status_or_and_ack_clears_current_bit() {
        let mut vm = VM::with_memory_size(8, 256);
        vm.instructions = vec![
            nop(),
            halt(),
            nop(),
            nop(),
            instr(0x0E, Register::Rv0 as u16, 99, 0), // handler: LI RV0, 99
            nop(), nop(), nop(),
        ];
        vm.state = VMState::Running;
        vm.handle_mmio_write(HDR_IRQ_VECTOR_BANK, 0);
        vm.handle_mmio_write(HDR_IRQ_VECTOR_OFF, 4);
        vm.handle_mmio_write(HDR_IRQ_ENABLE, 0x0005); // bits 0 and 2
        vm.handle_mmio_write(HDR_IRQ_STATUS, 0x0004); // bit 2
        vm.handle_mmio_write(HDR_IRQ_STATUS, 0x0001); // OR bit 0
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_STATUS), Some(0x0005));

        vm.step().unwrap();
        // Lowest enabled pending bit is 0 → cause 1, handler at 4 executed
        assert_eq!(vm.registers[Register::Pc as usize], 5);
        assert_eq!(vm.registers[Register::Ra as usize], 0);
        assert_eq!(vm.registers[Register::Rv0 as usize], 99);
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_BUSY), Some(1));
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_CAUSE), Some(1));

        vm.handle_mmio_write(HDR_IRQ_BUSY, 0); // write 0 does not ACK
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_BUSY), Some(1));
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_CAUSE), Some(1));

        vm.handle_mmio_write(HDR_IRQ_BUSY, 1); // ACK
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_BUSY), Some(0));
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_CAUSE), Some(0));
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_STATUS), Some(0x0004)); // bit 2 still pending
    }

    #[test]
    fn irq_does_not_nest_while_busy() {
        let mut vm = VM::with_memory_size(8, 256);
        vm.instructions = vec![
            nop(),
            halt(),
            nop(),
            nop(),
            nop(), // 4 handler
            nop(),
            nop(),
            nop(),
        ];
        vm.state = VMState::Running;
        vm.handle_mmio_write(HDR_IRQ_VECTOR_OFF, 4);
        vm.handle_mmio_write(HDR_IRQ_ENABLE, 0x0003);
        vm.handle_mmio_write(HDR_IRQ_STATUS, 0x0003);

        vm.step().unwrap();
        assert_eq!(vm.registers[Register::Pc as usize], 5);
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_CAUSE), Some(1));
        let ra = vm.registers[Register::Ra as usize];

        vm.step().unwrap();
        // Still in handler; did not re-enter at 4
        assert_eq!(vm.registers[Register::Pc as usize], 6);
        assert_eq!(vm.registers[Register::Ra as usize], ra);
        assert_eq!(vm.handle_mmio_read(HDR_IRQ_BUSY), Some(1));
    }
}

impl Drop for VM {
    fn drop(&mut self) {
        // Ensure terminal is restored when VM is dropped
        if self.tty_input_enabled {
            self.disable_tty_input();
        }
        if self.terminal_raw_mode {
            if self.display_mode == DISP_TTY {
                self.exit_tty_mode();
            } else if self.display_mode == DISP_TEXT40 {
                self.exit_text40_mode();
            }
        }
        
        // Flush storage if present
        if let Some(ref mut storage) = self.storage {
            storage.flush();
        }
    }
}