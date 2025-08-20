use crate::constants::*;
use super::VM;

impl VM {
    /// Handle MMIO reads for special addresses in bank 0
    pub(super) fn handle_mmio_read(&mut self, addr: usize) -> Option<u16> {
        // Only handle bank 0 MMIO addresses
        if addr >= TEXT40_BASE_WORD {
            return None; // Regular memory access for VRAM and beyond
        }
        
        match addr {
            HDR_TTY_OUT => Some(0), // Write-only, return 0
            HDR_TTY_STATUS => {
                let value = if self.output_ready { TTY_READY } else { 0 };
                self.memory[HDR_TTY_STATUS] = value;
                Some(value)
            },
            HDR_TTY_IN_POP => {
                // Enable TTY input mode on first access
                if !self.tty_input_enabled && !self.debug_mode && !self.verbose {
                    self.enable_tty_input();
                }
                
                // Pop a byte from input buffer
                let value = if let Some(byte) = self.input_buffer.pop_front() {
                    byte as u16
                } else {
                    0 // Return 0 if buffer is empty
                };
                // Store the popped value in memory
                self.memory[HDR_TTY_IN_POP] = value;
                Some(value)
            },
            HDR_TTY_IN_STATUS => {
                // Enable TTY input mode on first access
                if !self.tty_input_enabled && !self.debug_mode && !self.verbose {
                    self.enable_tty_input();
                }
                
                let value = if !self.input_buffer.is_empty() { TTY_HAS_BYTE } else { 0 };
                self.memory[HDR_TTY_IN_STATUS] = value;
                Some(value)
            },
            HDR_RNG => {
                // Simple LCG: next = (a * prev + c) mod m
                self.rng_state = self.rng_state.wrapping_mul(1664525).wrapping_add(1013904223);
                let value = (self.rng_state >> 16) as u16;
                // Store the generated value in memory
                self.memory[HDR_RNG] = value;
                Some(value) // Return the value
            },
            HDR_RNG_SEED => {
                // Return the low 16 bits of current seed
                let value = (self.rng_state & 0xFFFF) as u16;
                self.memory[HDR_RNG_SEED] = value;
                Some(value)
            },
            HDR_DISP_MODE => {
                self.memory[HDR_DISP_MODE] = self.display_mode;
                Some(self.display_mode)
            },
            HDR_DISP_STATUS => {
                let mut status = 0;
                if self.output_ready { status |= DISP_READY; }
                if self.display_flush_done { status |= DISP_FLUSH_DONE; }
                self.memory[HDR_DISP_STATUS] = status;
                Some(status)
            },
            HDR_DISP_CTL => {
                let value = if self.display_enabled { DISP_ENABLE } else { 0 };
                self.memory[HDR_DISP_CTL] = value;
                Some(value)
            },
            HDR_DISP_FLUSH => Some(0), // Write-only, return 0
            HDR_KEY_UP | HDR_KEY_DOWN | HDR_KEY_LEFT | HDR_KEY_RIGHT | HDR_KEY_Z | HDR_KEY_X => {
                let value = if self.display_mode == DISP_RGB565 {
                    // For RGB565 mode, read keyboard state from the display window
                    if let Some(ref display) = self.rgb565_display {
                        let state = display.get_state();
                        let s = state.lock().unwrap();
                        match addr {
                            HDR_KEY_UP => if s.key_up { 1 } else { 0 },
                            HDR_KEY_DOWN => if s.key_down { 1 } else { 0 },
                            HDR_KEY_LEFT => if s.key_left { 1 } else { 0 },
                            HDR_KEY_RIGHT => if s.key_right { 1 } else { 0 },
                            HDR_KEY_Z => if s.key_z { 1 } else { 0 },
                            HDR_KEY_X => if s.key_x { 1 } else { 0 },
                            _ => 0,
                        }
                    } else {
                        0
                    }
                } else if self.display_mode == DISP_TEXT40 && self.terminal_raw_mode {
                    // For TEXT40 mode, poll keyboard from terminal
                    self.poll_keyboard();
                    
                    // Auto-clear keys after they've been read a few times
                    // This prevents keys from getting "stuck" but allows for repeated reads
                    self.keyboard_state.last_read_counter += 1;
                    if self.keyboard_state.last_read_counter > 10 {
                        // Clear the keyboard state after ~10 reads without new input
                        self.keyboard_state = super::state::KeyboardState::default();
                    }
                    
                    // Return the appropriate key state
                    match addr {
                        HDR_KEY_UP => if self.keyboard_state.up { 1 } else { 0 },
                        HDR_KEY_DOWN => if self.keyboard_state.down { 1 } else { 0 },
                        HDR_KEY_LEFT => if self.keyboard_state.left { 1 } else { 0 },
                        HDR_KEY_RIGHT => if self.keyboard_state.right { 1 } else { 0 },
                        HDR_KEY_Z => if self.keyboard_state.z { 1 } else { 0 },
                        HDR_KEY_X => if self.keyboard_state.x { 1 } else { 0 },
                        _ => 0,
                    }
                } else {
                    0  // No keyboard input in other modes
                };
                
                self.memory[addr] = value;
                Some(value)
            },
            HDR_DISP_RESOLUTION => {
                self.memory[HDR_DISP_RESOLUTION] = self.display_resolution;
                Some(self.display_resolution)
            },
            17..=31 => Some(0), // Reserved addresses return 0
            _ => None, // Not an MMIO address
        }
    }
    
    /// Handle MMIO writes for special addresses in bank 0
    pub(super) fn handle_mmio_write(&mut self, addr: usize, value: u16) -> bool {
        // Only handle bank 0 MMIO addresses
        if addr >= TEXT40_BASE_WORD {
            return false; // Regular memory write for VRAM and beyond
        }
        
        match addr {
            HDR_TTY_OUT => {
                // Output low byte to stdout
                let byte = (value & 0xFF) as u8;
                
                // Print immediately to stdout for real-time effect
                use std::io::{self, Write};
                
                // If we're in raw mode and outputting a newline, also output carriage return
                if self.tty_input_enabled && byte == b'\n' {
                    // Output \r\n for proper line ending in raw mode
                    let _ = io::stdout().write_all(b"\r\n");
                } else {
                    let _ = io::stdout().write_all(&[byte]);
                }
                let _ = io::stdout().flush();
                
                // Also store in buffer for compatibility
                self.output_buffer.push_back(byte);
                self.output_ready = false;
                // Simulate output delay (will be set ready in next cycle)
                true
            },
            HDR_TTY_STATUS => true, // Read-only, ignore write
            HDR_TTY_IN_POP => true, // Read-only, ignore write
            HDR_TTY_IN_STATUS => true, // Read-only, ignore write
            HDR_RNG => true, // Read-only, ignore write
            HDR_RNG_SEED => {
                // Set the low 16 bits of the RNG seed
                // Keep high 16 bits unchanged to maintain some state
                self.rng_state = (self.rng_state & 0xFFFF0000) | (value as u32);
                // Store the value in memory for read-back
                self.memory[HDR_RNG_SEED] = value;
                true
            },
            HDR_DISP_MODE => {
                let new_mode = value & 0x3; // Support 4 modes (0-3)
                if new_mode != self.display_mode {
                    // Mode change - handle terminal setup/teardown
                    self.handle_display_mode_change(new_mode);
                }
                self.display_mode = new_mode;
                true
            },
            HDR_DISP_STATUS => true, // Read-only, ignore write
            HDR_DISP_CTL => {
                // Handle control bits
                if value & DISP_ENABLE != 0 {
                    self.display_enabled = true;
                }
                if value & DISP_CLEAR != 0 {
                    // Clear VRAM (edge-triggered)
                    for i in TEXT40_BASE_WORD..=TEXT40_LAST_WORD {
                        if i < self.memory.len() {
                            self.memory[i] = 0;
                        }
                    }
                    // Auto-clear the CLEAR bit (edge-triggered)
                }
                true
            },
            HDR_DISP_FLUSH => {
                if value != 0 {
                    self.display_flush_done = false;
                    // Trigger display rendering based on mode
                    if self.display_mode == DISP_TEXT40 {
                        self.flush_text40_display();
                    } else if self.display_mode == DISP_RGB565 {
                        self.flush_rgb565_display();
                    }
                    self.display_flush_done = true;
                }
                true
            },
            HDR_KEY_UP | HDR_KEY_DOWN | HDR_KEY_LEFT | HDR_KEY_RIGHT | HDR_KEY_Z | HDR_KEY_X => {
                true // Read-only keyboard flags, ignore writes
            },
            HDR_DISP_RESOLUTION => {
                // Set display resolution for RGB565 mode
                self.display_resolution = value;
                self.memory[HDR_DISP_RESOLUTION] = value;
                true
            },
            17..=31 => true, // Reserved addresses, ignore writes
            _ => false, // Not an MMIO address
        }
    }
}