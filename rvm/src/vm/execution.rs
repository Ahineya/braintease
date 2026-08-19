use super::{VM, VMState};
use super::instruction::Instr;
use crate::constants::*;
use ripple_asm::Register;

impl VM {
    pub(super) fn execute_instruction(&mut self, instr: Instr) -> Result<(), String> {
        // R0 always reads as 0
        self.registers[Register::R0 as usize] = 0;
        
        match instr.opcode {
            0x00 => {
                // HALT (operands ignored)
                self.state = VMState::Halted;
            },
            0x42 => {
                // NOP
            },
            
            // ALU R-type operations
            0x01 => { // ADD
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let rt = instr.word3 as usize;
                if rd < 32 && rs < 32 && rt < 32 {
                    self.registers[rd] = self.registers[rs].wrapping_add(self.registers[rt]);
                }
            },
            0x02 => { // SUB
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let rt = instr.word3 as usize;
                if rd < 32 && rs < 32 && rt < 32 {
                    self.registers[rd] = self.registers[rs].wrapping_sub(self.registers[rt]);
                }
            },
            0x03 => { // AND
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let rt = instr.word3 as usize;
                if rd < 32 && rs < 32 && rt < 32 {
                    self.registers[rd] = self.registers[rs] & self.registers[rt];
                }
            },
            0x04 => { // OR
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let rt = instr.word3 as usize;
                if rd < 32 && rs < 32 && rt < 32 {
                    self.registers[rd] = self.registers[rs] | self.registers[rt];
                }
            },
            0x05 => { // XOR
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let rt = instr.word3 as usize;
                if rd < 32 && rs < 32 && rt < 32 {
                    self.registers[rd] = self.registers[rs] ^ self.registers[rt];
                }
            },
            0x06 => { // SLL
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let rt = instr.word3 as usize;
                if rd < 32 && rs < 32 && rt < 32 {
                    let shift = (self.registers[rt] & 15) as u32;
                    self.registers[rd] = self.registers[rs].wrapping_shl(shift);
                }
            },
            0x07 => { // SRL
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let rt = instr.word3 as usize;
                if rd < 32 && rs < 32 && rt < 32 {
                    let shift = (self.registers[rt] & 15) as u32;
                    self.registers[rd] = self.registers[rs].wrapping_shr(shift);
                }
            },
            0x08 => { // SLT (signed compare)
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let rt = instr.word3 as usize;
                if rd < 32 && rs < 32 && rt < 32 {
                    let rs_val = self.registers[rs] as i16;
                    let rt_val = self.registers[rt] as i16;
                    self.registers[rd] = if rs_val < rt_val { 1 } else { 0 };
                }
            },
            0x09 => { // SLTU (unsigned compare)
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let rt = instr.word3 as usize;
                if rd < 32 && rs < 32 && rt < 32 {
                    self.registers[rd] = if self.registers[rs] < self.registers[rt] { 1 } else { 0 };
                }
            },
            
            // ALU I-type operations
            0x0A => { // ADDI
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let imm = instr.word3;
                if rd < 32 && rs < 32 {
                    self.registers[rd] = self.registers[rs].wrapping_add(imm);
                }
            },
            0x0B => { // ANDI
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let imm = instr.word3;
                if rd < 32 && rs < 32 {
                    self.registers[rd] = self.registers[rs] & imm;
                }
            },
            0x0C => { // ORI
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let imm = instr.word3;
                if rd < 32 && rs < 32 {
                    self.registers[rd] = self.registers[rs] | imm;
                }
            },
            0x0D => { // XORI
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let imm = instr.word3;
                if rd < 32 && rs < 32 {
                    self.registers[rd] = self.registers[rs] ^ imm;
                }
            },
            0x0E => { // LI
                let rd = instr.word1 as usize;
                let imm = instr.word2;
                if rd < 32 {
                    self.registers[rd] = imm;
                }
            },
            0x0F => { // SLLI
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let imm = instr.word3 as u32;
                if rd < 32 && rs < 32 {
                    self.registers[rd] = self.registers[rs].wrapping_shl(imm & 15);
                }
            },
            0x10 => { // SRLI
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let imm = instr.word3 as u32;
                if rd < 32 && rs < 32 {
                    self.registers[rd] = self.registers[rs].wrapping_shr(imm & 15);
                }
            },
            
            // Memory operations
            0x11 => { // LOAD - rd = memory[bank_reg][addr_reg]
                let rd = instr.word1 as usize;
                let bank_reg = instr.word2 as usize;
                let addr_reg = instr.word3 as usize;
                if rd < 32 && bank_reg < 32 && addr_reg < 32 {
                    let bank_val = self.registers[bank_reg];
                    let addr_val = self.registers[addr_reg];
                    
                    // Check if this is a bank 0 MMIO access
                    if bank_val == 0 && addr_val < TEXT40_LAST_WORD as u16 + 1 {
                        // Try MMIO read first
                        if let Some(value) = self.handle_mmio_read(addr_val as usize) {
                            self.registers[rd] = value;
                        } else {
                            // Regular memory read for VRAM and other bank 0 addresses
                            self.registers[rd] = self.memory[addr_val as usize];
                        }
                    } else if bank_val == 0 && self.display_mode == DISP_RGB565 {
                        // Check if this is RGB565 framebuffer access
                        if let Some(ref display) = self.rgb565_display {
                            if let Some(value) = display.read_memory(addr_val as usize) {
                                self.registers[rd] = value;
                            } else {
                                // Regular memory access
                                let mem_addr = addr_val as usize;
                                if mem_addr < self.memory.len() {
                                    self.registers[rd] = self.memory[mem_addr];
                                } else {
                                    return Err(format!("LOAD: memory address out of bounds: {mem_addr}"));
                                }
                            }
                        } else {
                            self.registers[rd] = 0;
                        }
                    } else {
                        // Regular memory access for non-bank-0
                        let mem_addr = (bank_val as usize * self.bank_size as usize) + addr_val as usize;
                        if mem_addr < self.memory.len() {
                            self.registers[rd] = self.memory[mem_addr];
                        } else {
                            return Err(format!("LOAD: memory address out of bounds: {mem_addr}"));
                        }
                    }
                }
            },
            0x12 => { // STORE - memory[bank_reg][addr_reg] = rs
                let rs = instr.word1 as usize;
                let bank_reg = instr.word2 as usize;
                let addr_reg = instr.word3 as usize;
                if rs < 32 && bank_reg < 32 && addr_reg < 32 {
                    let bank_val = self.registers[bank_reg];
                    let addr_val = self.registers[addr_reg];
                    let value = self.registers[rs];
                    
                    // Check if this is a bank 0 MMIO access
                    if bank_val == 0 && addr_val < TEXT40_LAST_WORD as u16 + 1 {
                        // Try MMIO write first
                        if !self.handle_mmio_write(addr_val as usize, value) {
                            // Regular memory write for VRAM and other bank 0 addresses
                            self.memory[addr_val as usize] = value;
                        }
                    } else if bank_val == 0 && self.display_mode == DISP_RGB565 {
                        // Check if this is RGB565 framebuffer access
                        if let Some(ref mut display) = self.rgb565_display {
                            display.write_memory(addr_val as usize, value);
                        } else {
                            // Regular memory write
                            let mem_addr = addr_val as usize;
                            if mem_addr < self.memory.len() {
                                self.memory[mem_addr] = value;
                            } else {
                                return Err(format!("STORE: memory address out of bounds: {mem_addr}"));
                            }
                        }
                    } else {
                        // Regular memory access for non-bank-0
                        let mem_addr = (bank_val as usize * self.bank_size as usize) + addr_val as usize;
                        if mem_addr < self.memory.len() {
                            self.memory[mem_addr] = value;
                        } else {
                            return Err(format!("STORE: memory address out of bounds: {mem_addr}"));
                        }
                    }
                }
            },
            
            // Control flow
            0x13 => { // JAL rd, bank, addr
                // Link pair is (rd, RAB). rd=R0 means a non-linking jump, same as JALR R0:
                // do not clobber RA or RAB (rOS vector tables rely on this).
                let rd = instr.word1 as usize;
                let target_bank = instr.word2;
                let target_addr = instr.word3;
                let ret_pc = self.registers[Register::Pc as usize].wrapping_add(1);
                let ret_pcb = self.registers[Register::Pcb as usize];
                if rd < 32 && rd != Register::R0 as usize {
                    self.registers[rd] = ret_pc;
                    self.registers[Register::Rab as usize] = ret_pcb;
                }
                self.registers[Register::Pcb as usize] = target_bank;
                self.registers[Register::Pc as usize] = target_addr;
                self.skip_pc_increment = true;
            },
            0x14 => { // JALR rd, bank_reg, addr_reg
                // Snapshot targets before any write (bank_reg may be RAB).
                let rd = instr.word1 as usize;
                let bank_reg = instr.word2 as usize;
                let addr_reg = instr.word3 as usize;
                if rd < 32 && bank_reg < 32 && addr_reg < 32 {
                    let new_pc = self.registers[addr_reg];
                    let new_pcb = self.registers[bank_reg];
                    let ret_pc = self.registers[Register::Pc as usize].wrapping_add(1);
                    let ret_pcb = self.registers[Register::Pcb as usize];
                    if rd != Register::R0 as usize {
                        self.registers[rd] = ret_pc;
                        self.registers[Register::Rab as usize] = ret_pcb;
                    }
                    self.registers[Register::Pcb as usize] = new_pcb;
                    self.registers[Register::Pc as usize] = new_pc;
                    self.skip_pc_increment = true;
                }
            },
            0x15 => { // BEQ
                let rs = instr.word1 as usize;
                let rt = instr.word2 as usize;
                let offset = instr.word3 as i16;
                if rs < 32 && rt < 32
                    && self.registers[rs] == self.registers[rt] {
                        let new_pc = (self.registers[Register::Pc as usize] as i16).wrapping_add(offset);
                        self.registers[Register::Pc as usize] = new_pc as u16;
                        self.skip_pc_increment = true;
                    }
            },
            0x16 => { // BNE
                let rs = instr.word1 as usize;
                let rt = instr.word2 as usize;
                let offset = instr.word3 as i16;
                if rs < 32 && rt < 32
                    && self.registers[rs] != self.registers[rt] {
                        let new_pc = (self.registers[Register::Pc as usize] as i16).wrapping_add(offset);
                        self.registers[Register::Pc as usize] = new_pc as u16;
                        self.skip_pc_increment = true;
                    }
            },
            0x17 => { // BLT
                let rs = instr.word1 as usize;
                let rt = instr.word2 as usize;
                let offset = instr.word3 as i16;
                if rs < 32 && rt < 32 {
                    let rs_val = self.registers[rs] as i16;
                    let rt_val = self.registers[rt] as i16;
                    if rs_val < rt_val {
                        let new_pc = (self.registers[Register::Pc as usize] as i16).wrapping_add(offset);
                        self.registers[Register::Pc as usize] = new_pc as u16;
                        self.skip_pc_increment = true;
                    }
                }
            },
            0x18 => { // BGE
                let rs = instr.word1 as usize;
                let rt = instr.word2 as usize;
                let offset = instr.word3 as i16;
                if rs < 32 && rt < 32 {
                    let rs_val = self.registers[rs] as i16;
                    let rt_val = self.registers[rt] as i16;
                    if rs_val >= rt_val {
                        let new_pc = (self.registers[Register::Pc as usize] as i16).wrapping_add(offset);
                        self.registers[Register::Pc as usize] = new_pc as u16;
                        self.skip_pc_increment = true;
                    }
                }
            },
            
            0x19 => { // BRK - debugger breakpoint
                if self.debug_mode {
                    // In debug mode, just pause execution
                    self.state = VMState::Breakpoint;
                    // Don't print here - the debugger_ui will handle it
                } else {
                    // In normal mode, dump state and halt
                    self.dump_vm_state();
                    
                    // Halt execution
                    self.state = VMState::Halted;
                    return Ok(());
                }
            },
            
            // Multiplication and division
            0x1A => { // MUL
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let rt = instr.word3 as usize;
                if rd < 32 && rs < 32 && rt < 32 {
                    self.registers[rd] = self.registers[rs].wrapping_mul(self.registers[rt]);
                }
            },
            0x1B => { // DIV
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let rt = instr.word3 as usize;
                if rd < 32 && rs < 32 && rt < 32 {
                    if self.registers[rt] != 0 {
                        self.registers[rd] = self.registers[rs] / self.registers[rt];
                    } else {
                        self.registers[rd] = 0; // Division by zero results in 0
                    }
                }
            },
            0x1C => { // MOD
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let rt = instr.word3 as usize;
                if rd < 32 && rs < 32 && rt < 32 {
                    if self.registers[rt] != 0 {
                        self.registers[rd] = self.registers[rs] % self.registers[rt];
                    } else {
                        self.registers[rd] = 0; // Modulo by zero results in 0
                    }
                }
            },
            0x1D => { // MULI
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let imm = instr.word3;
                if rd < 32 && rs < 32 {
                    self.registers[rd] = self.registers[rs].wrapping_mul(imm);
                }
            },
            0x1E => { // DIVI
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let imm = instr.word3;
                if rd < 32 && rs < 32 {
                    if imm != 0 {
                        self.registers[rd] = self.registers[rs] / imm;
                    } else {
                        self.registers[rd] = 0;
                    }
                }
            },
            0x1F => { // MODI
                let rd = instr.word1 as usize;
                let rs = instr.word2 as usize;
                let imm = instr.word3;
                if rd < 32 && rs < 32 {
                    if imm != 0 {
                        self.registers[rd] = self.registers[rs] % imm;
                    } else {
                        self.registers[rd] = 0;
                    }
                }
            },

            0x20 => { // LOADC - X0..X3 ← 4 cells of IMEM[bank_reg][addr_reg]
                let bank_reg = instr.word2 as usize;
                let addr_reg = instr.word3 as usize;
                if bank_reg < 32 && addr_reg < 32 {
                    let bank_val = self.registers[bank_reg];
                    let addr_val = self.registers[addr_reg];
                    let idx = (bank_val as usize * self.bank_size as usize) + addr_val as usize;
                    if idx >= self.instructions.len() {
                        return Err(format!("LOADC: instruction address out of bounds: bank={bank_val}, addr={addr_val}, idx={idx}"));
                    }
                    let loaded = self.instructions[idx];
                    self.registers[Register::X0 as usize] = loaded.opcode as u16;
                    self.registers[Register::X1 as usize] = loaded.word1;
                    self.registers[Register::X2 as usize] = loaded.word2;
                    self.registers[Register::X3 as usize] = loaded.word3;
                }
            },
            0x21 => { // STORC - IMEM[bank_reg][addr_reg] ← X0..X3 (4 instruction cells)
                let bank_reg = instr.word2 as usize;
                let addr_reg = instr.word3 as usize;
                if bank_reg < 32 && addr_reg < 32 {
                    let bank_val = self.registers[bank_reg];
                    let addr_val = self.registers[addr_reg];
                    let idx = (bank_val as usize * self.bank_size as usize) + addr_val as usize;
                    const MAX_IMEM: usize = 64 * 65536;
                    if idx >= MAX_IMEM {
                        return Err(format!("STORC: instruction address out of bounds: bank={bank_val}, addr={addr_val}, idx={idx}"));
                    }
                    if idx >= self.instructions.len() {
                        let nop = Instr {
                            opcode: OP_NOP,
                            word0: OP_NOP,
                            word1: 0,
                            word2: 0,
                            word3: 0,
                        };
                        let bs = self.bank_size.max(1) as usize;
                        let bank_end = (bank_val as usize + 1).saturating_mul(bs);
                        let new_len = (idx + 1).max(bank_end).min(MAX_IMEM);
                        self.instructions.resize(new_len, nop);
                    }
                    let x0 = self.registers[Register::X0 as usize];
                    let x1 = self.registers[Register::X1 as usize];
                    let x2 = self.registers[Register::X2 as usize];
                    let x3 = self.registers[Register::X3 as usize];
                    let opcode = (x0 & 0xFF) as u8;
                    self.instructions[idx] = Instr {
                        opcode,
                        word0: opcode,
                        word1: x1,
                        word2: x2,
                        word3: x3,
                    };
                }
            },
            
            _ => {
                return Err(format!("Unknown opcode: 0x{:02X}", instr.opcode));
            }
        }
        
        // R0 always reads as 0 (enforce after every instruction)
        self.registers[Register::R0 as usize] = 0;
        
        // Simulate output ready after some cycles (instant for now)
        if !self.output_ready {
            self.output_ready = true;
        }
        
        Ok(())
    }
    
    fn dump_vm_state(&self) {
        eprintln!("\n=== BRK: VM State Dump ===");
        eprintln!("PC: {} (bank: {})", self.registers[Register::Pc as usize], self.registers[Register::Pcb as usize]);
        eprintln!("RA: {} (bank: {})", self.registers[Register::Ra as usize], self.registers[Register::Rab as usize]);
        
        eprintln!("\nRegisters:");
        eprintln!("  R0 (ZR):  0x{:04X} ({})", self.registers[Register::R0 as usize], self.registers[Register::R0 as usize]);
        eprintln!("  R1 (PC):  0x{:04X} ({})", self.registers[Register::Pc as usize], self.registers[Register::Pc as usize]);
        eprintln!("  R2 (PCB): 0x{:04X} ({})", self.registers[Register::Pcb as usize], self.registers[Register::Pcb as usize]);
        eprintln!("  R3 (RA):  0x{:04X} ({})", self.registers[Register::Ra as usize], self.registers[Register::Ra as usize]);
        eprintln!("  R4 (RAB): 0x{:04X} ({})", self.registers[Register::Rab as usize], self.registers[Register::Rab as usize]);
        eprintln!("  R5 (RV0): 0x{:04X} ({})", self.registers[Register::Rv0 as usize], self.registers[Register::Rv0 as usize]);
        eprintln!("  R6 (RV1): 0x{:04X} ({})", self.registers[Register::Rv1 as usize], self.registers[Register::Rv1 as usize]);
        eprintln!("  R7 (A0):  0x{:04X} ({})", self.registers[Register::A0 as usize], self.registers[Register::A0 as usize]);
        eprintln!("  R8 (A1):  0x{:04X} ({})", self.registers[Register::A1 as usize], self.registers[Register::A1 as usize]);
        eprintln!("  R9 (A2):  0x{:04X} ({})", self.registers[Register::A2 as usize], self.registers[Register::A2 as usize]);
        eprintln!("  R10 (A3): 0x{:04X} ({})", self.registers[Register::A3 as usize], self.registers[Register::A3 as usize]);
        eprintln!("  R11 (X0): 0x{:04X} ({})", self.registers[Register::X0 as usize], self.registers[Register::X0 as usize]);
        eprintln!("  R12 (X1): 0x{:04X} ({})", self.registers[Register::X1 as usize], self.registers[Register::X1 as usize]);
        eprintln!("  R13 (X2): 0x{:04X} ({})", self.registers[Register::X2 as usize], self.registers[Register::X2 as usize]);
        eprintln!("  R14 (X3): 0x{:04X} ({})", self.registers[Register::X3 as usize], self.registers[Register::X3 as usize]);
        eprintln!("  R15 (T0): 0x{:04X} ({})", self.registers[Register::T0 as usize], self.registers[Register::T0 as usize]);
        eprintln!("  R16 (T1): 0x{:04X} ({})", self.registers[Register::T1 as usize], self.registers[Register::T1 as usize]);
        eprintln!("  R17 (T2): 0x{:04X} ({})", self.registers[Register::T2 as usize], self.registers[Register::T2 as usize]);
        eprintln!("  R18 (T3): 0x{:04X} ({})", self.registers[Register::T3 as usize], self.registers[Register::T3 as usize]);
        eprintln!("  R19 (T4): 0x{:04X} ({})", self.registers[Register::T4 as usize], self.registers[Register::T4 as usize]);
        eprintln!("  R20 (T5): 0x{:04X} ({})", self.registers[Register::T5 as usize], self.registers[Register::T5 as usize]);
        eprintln!("  R21 (T6): 0x{:04X} ({})", self.registers[Register::T6 as usize], self.registers[Register::T6 as usize]);
        eprintln!("  R22 (T7): 0x{:04X} ({})", self.registers[Register::T7 as usize], self.registers[Register::T7 as usize]);
        eprintln!("  R23 (S0): 0x{:04X} ({})", self.registers[Register::S0 as usize], self.registers[Register::S0 as usize]);
        eprintln!("  R24 (S1): 0x{:04X} ({})", self.registers[Register::S1 as usize], self.registers[Register::S1 as usize]);
        eprintln!("  R25 (S2): 0x{:04X} ({})", self.registers[Register::S2 as usize], self.registers[Register::S2 as usize]);
        eprintln!("  R26 (S3): 0x{:04X} ({})", self.registers[Register::S3 as usize], self.registers[Register::S3 as usize]);
        eprintln!("  R27 (SC): 0x{:04X} ({})", self.registers[Register::Sc as usize], self.registers[Register::Sc as usize]);
        eprintln!("  R28 (SB): 0x{:04X} ({})", self.registers[Register::Sb as usize], self.registers[Register::Sb as usize]);
        eprintln!("  R29 (SP): 0x{:04X} ({})", self.registers[Register::Sp as usize], self.registers[Register::Sp as usize]);
        eprintln!("  R30 (FP): 0x{:04X} ({})", self.registers[Register::Fp as usize], self.registers[Register::Fp as usize]);
        eprintln!("  R31 (GP): 0x{:04X} ({})", self.registers[Register::Gp as usize], self.registers[Register::Gp as usize]);
        
        eprintln!("\nMemory (first {} words):", DEBUG_MEMORY_DISPLAY_WORDS);
        for i in (0..DEBUG_MEMORY_DISPLAY_WORDS.min(self.memory.len())).step_by(DEBUG_MEMORY_WORDS_PER_LINE) {
            eprint!("  {:04X}: ", i);
            for j in 0..DEBUG_MEMORY_WORDS_PER_LINE {
                if i + j < self.memory.len() {
                    eprint!("{:04X} ", self.memory[i + j]);
                }
            }
            eprintln!();
        }
        
        eprintln!("\nInstruction at PC:");
        let pc_val = self.registers[Register::Pc as usize] as usize;
        if pc_val < self.instructions.len() {
            let inst = &self.instructions[pc_val];
            eprintln!("  [{:04X}] opcode: 0x{:02X}, w1: 0x{:04X}, w2: 0x{:04X}, w3: 0x{:04X}", 
                pc_val, inst.opcode, inst.word1, inst.word2, inst.word3);
        }
        
        eprintln!("=========================\n");
    }
    
    pub(super) fn print_instruction(&self, instr: &Instr) {
        match instr.opcode {
            0x00 => eprintln!("HALT"),
            0x42 => eprintln!("NOP"),
            0x01 => eprintln!("ADD R{}, R{}, R{}", instr.word1, instr.word2, instr.word3),
            0x02 => eprintln!("SUB R{}, R{}, R{}", instr.word1, instr.word2, instr.word3),
            0x03 => eprintln!("AND R{}, R{}, R{}", instr.word1, instr.word2, instr.word3),
            0x04 => eprintln!("OR R{}, R{}, R{}", instr.word1, instr.word2, instr.word3),
            0x05 => eprintln!("XOR R{}, R{}, R{}", instr.word1, instr.word2, instr.word3),
            0x06 => eprintln!("SLL R{}, R{}, R{}", instr.word1, instr.word2, instr.word3),
            0x07 => eprintln!("SRL R{}, R{}, R{}", instr.word1, instr.word2, instr.word3),
            0x08 => eprintln!("SLT R{}, R{}, R{}", instr.word1, instr.word2, instr.word3),
            0x09 => eprintln!("SLTU R{}, R{}, R{}", instr.word1, instr.word2, instr.word3),
            0x0A => eprintln!("ADDI R{}, R{}, {}", instr.word1, instr.word2, instr.word3),
            0x0B => eprintln!("ANDI R{}, R{}, {}", instr.word1, instr.word2, instr.word3),
            0x0C => eprintln!("ORI R{}, R{}, {}", instr.word1, instr.word2, instr.word3),
            0x0D => eprintln!("XORI R{}, R{}, {}", instr.word1, instr.word2, instr.word3),
            0x0E => eprintln!("LI R{}, {}", instr.word1, instr.word2),
            0x0F => eprintln!("SLLI R{}, R{}, {}", instr.word1, instr.word2, instr.word3),
            0x10 => eprintln!("SRLI R{}, R{}, {}", instr.word1, instr.word2, instr.word3),
            0x11 => {
                eprintln!("LOAD R{}, R{}, R{} ; R{}=mem[R{}*{}+R{}]={}", 
                    instr.word1, instr.word2, instr.word3,
                    instr.word1, instr.word2, self.bank_size, instr.word3,
                    if instr.word1 < 32 && instr.word2 < 32 && instr.word3 < 32 {
                        let addr = (self.registers[instr.word2 as usize] as usize * self.bank_size as usize) 
                                 + self.registers[instr.word3 as usize] as usize;
                        if addr < self.memory.len() {
                            self.memory[addr]
                        } else {
                            0
                        }
                    } else {
                        0
                    });
            },
            0x12 => {
                eprintln!("STORE R{}, R{}, R{} ; mem[R{}*{}+R{}]=R{}={}", 
                    instr.word1, instr.word2, instr.word3,
                    instr.word2, self.bank_size, instr.word3, instr.word1,
                    if instr.word1 < 32 {
                        self.registers[instr.word1 as usize]
                    } else {
                        0
                    });
            },
            0x13 => eprintln!("JAL R{}, bank {}, addr {}", instr.word1, instr.word2, instr.word3),
            0x14 => eprintln!("JALR R{}, R{} (bank), R{} (addr)", instr.word1, instr.word2, instr.word3),
            0x15 => eprintln!("BEQ R{}, R{}, {} ; if R{}==R{} goto PC+{}", 
                instr.word1, instr.word2, instr.word3 as i16,
                instr.word1, instr.word2, instr.word3 as i16),
            0x16 => eprintln!("BNE R{}, R{}, {} ; if R{}!=R{} goto PC+{}", 
                instr.word1, instr.word2, instr.word3 as i16,
                instr.word1, instr.word2, instr.word3 as i16),
            0x17 => eprintln!("BLT R{}, R{}, {}", instr.word1, instr.word2, instr.word3 as i16),
            0x18 => eprintln!("BGE R{}, R{}, {}", instr.word1, instr.word2, instr.word3 as i16),
            0x19 => eprintln!("BRK"),
            0x1A => eprintln!("MUL R{}, R{}, R{}", instr.word1, instr.word2, instr.word3),
            0x1B => eprintln!("DIV R{}, R{}, R{}", instr.word1, instr.word2, instr.word3),
            0x1C => eprintln!("MOD R{}, R{}, R{}", instr.word1, instr.word2, instr.word3),
            0x1D => eprintln!("MULI R{}, R{}, {}", instr.word1, instr.word2, instr.word3),
            0x1E => eprintln!("DIVI R{}, R{}, {}", instr.word1, instr.word2, instr.word3),
            0x1F => eprintln!("MODI R{}, R{}, {}", instr.word1, instr.word2, instr.word3),
            0x20 => eprintln!("LOADC R{}, R{} (bank), R{} (addr)", instr.word1, instr.word2, instr.word3),
            0x21 => eprintln!("STORC R{}, R{} (bank), R{} (addr)", instr.word1, instr.word2, instr.word3),
            _ => eprintln!("UNKNOWN 0x{:02X}", instr.opcode),
        }
    }
}