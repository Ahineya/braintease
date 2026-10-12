use crate::constants::*;
use crate::display_rgb565::RGB565Display;
use super::VM;
use crate::display_text40::Text40Snapshot;
use std::io;

impl VM {
    /// Calculate the data section offset based on display mode and resolution
    pub(super) fn get_data_section_offset(&self) -> usize {
        match self.display_mode {
            DISP_RGB565 => {
                // For RGB565, data section must come after both framebuffers
                let width = ((self.display_resolution >> 8) & 0xFF) as usize;
                let height = (self.display_resolution & 0xFF) as usize;
                
                if width > 0 && height > 0 {
                    let pixels_per_buffer = width * height;
                    let total_framebuffer_words = pixels_per_buffer * 2; // Double-buffered
                    
                    // Data section starts after MMIO headers (32 words) and both framebuffers
                    32 + total_framebuffer_words
                } else {
                    // Invalid resolution, use default
                    TEXT40_LAST_WORD + 1
                }
            },
            DISP_TEXT40 => {
                // For TEXT40, data section starts after VRAM (word 1032)
                TEXT40_LAST_WORD + 1
            },
            _ => {
                // For OFF or TTY modes, use minimal offset (after MMIO headers)
                // But keep compatibility with existing binaries by using TEXT40 offset
                TEXT40_LAST_WORD + 1
            }
        }
    }
    
    /// Change devices without replacing the state shared with the window.
    pub(super) fn handle_display_mode_change(&mut self, new_mode: u16) -> Result<(), String> {
        if new_mode > DISP_RGB565_EXT {
            return Err(format!("Unsupported display mode {new_mode}"));
        }
        if new_mode == DISP_RGB565 {
            let display = self.rgb565_display.get_or_insert_with(RGB565Display::new);
            display.init((self.display_resolution >> 8) as u8,
                         self.display_resolution as u8, self.bank_size as usize)?;
        }
        match self.display_mode {
            DISP_TTY if self.terminal_raw_mode => self.exit_tty_mode(),
            DISP_TEXT40 if self.terminal_raw_mode => self.exit_text40_mode(),
            _ => {}
        }
        if new_mode != DISP_RGB565 {
            if let Some(display) = self.rgb565_display.as_mut() { display.disable(); }
        }
        match new_mode {
            DISP_TTY => self.enter_tty_mode(),
            DISP_TEXT40 => self.enter_text40_mode(),
            DISP_RGB565_EXT => {
                self.rgb565_display.get_or_insert_with(RGB565Display::new);
            }
            _ => {}
        }
        self.display_mode = new_mode;
        Ok(())
    }

    pub(super) fn flush_rgb565_display(&mut self) {
        if let Some(display) = self.rgb565_display.as_mut() {
            if display.flush() { self.presented_frames += 1; }
        }
    }

    /// Render TEXT40 display to terminal
    pub(super) fn flush_text40_display(&mut self) {
        if self.display_mode != DISP_TEXT40 || !self.display_enabled {
            return; // Display not in TEXT40 mode or not enabled
        }
        
        if self.terminal_raw_mode {
            // Render to actual terminal in TEXT40 mode
            let mut stderr = io::stderr();
            
            let snapshot = Text40Snapshot::from_memory(&self.memory, self.presented_frames);
            if let Err(error) = snapshot.render(&mut stderr) {
                self.device_error = Some(format!("Rendering TEXT40: {error}"));
            }
        } else if self.verbose {
            // Fallback to debug output
            eprintln!("\n=== TEXT40 Display (40x25) ===");
            for row in 0..25 {
                eprint!("  ");
                for col in 0..40 {
                    let addr = TEXT40_BASE_WORD + row * 40 + col;
                    if addr < self.memory.len() {
                        let cell = self.memory[addr];
                        let ch = (cell & 0xFF) as u8;
                        // Print ASCII character or '.' for non-printable
                        if (32..127).contains(&ch) {
                            eprint!("{}", ch as char);
                        } else if ch == 0 {
                            eprint!(" ");
                        } else {
                            eprint!(".");
                        }
                    }
                }
                eprintln!();
            }
            eprintln!("==============================\n");
        }
    }
}