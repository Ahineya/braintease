use super::VM;
use super::state::KeyboardState;
use crate::constants::*;
use crossterm::{
    terminal::{self, ClearType},
    cursor,
    style::ResetColor,
    event::{self, Event, KeyCode},
    ExecutableCommand,
};
use std::io::{self, Write, IsTerminal};
use std::time::Duration;

impl VM {
    /// Clear keyboard state flags
    pub(super) fn clear_keyboard_state(&mut self) {
        self.keyboard_state = KeyboardState::default();
    }
    
    /// Poll keyboard events and update keyboard state (non-blocking)
    pub(super) fn poll_keyboard(&mut self) {
        // Only poll keyboard in TEXT40 mode with terminal raw mode
        if self.display_mode != DISP_TEXT40 || !self.terminal_raw_mode {
            return;
        }
        
        // Poll for a single keyboard event with zero timeout (non-blocking)
        // Only process one event at a time to maintain state between reads
        if event::poll(Duration::from_millis(0)).unwrap_or(false) {
            if let Ok(Event::Key(key_event)) = event::read() {
                // Clear all keys first, then set the current key
                // This gives a "last key pressed" behavior
                self.keyboard_state.up = false;
                self.keyboard_state.down = false;
                self.keyboard_state.left = false;
                self.keyboard_state.right = false;
                self.keyboard_state.z = false;
                self.keyboard_state.x = false;
                self.keyboard_state.last_read_counter = 0;  // Reset counter on new input
                
                // Set the current key as pressed
                match key_event.code {
                    KeyCode::Up => {
                        self.keyboard_state.up = true;
                    },
                    KeyCode::Down => {
                        self.keyboard_state.down = true;
                    },
                    KeyCode::Left => {
                        self.keyboard_state.left = true;
                    },
                    KeyCode::Right => {
                        self.keyboard_state.right = true;
                    },
                    KeyCode::Char('z') | KeyCode::Char('Z') => {
                        self.keyboard_state.z = true;
                    },
                    KeyCode::Char('x') | KeyCode::Char('X') => {
                        self.keyboard_state.x = true;
                    },
                    _ => {
                        // For other keys, don't change state
                    }
                }
            }
        }
        // If no event, keep the current state (don't clear)
    }
    
    /// Enable TTY input mode (enable raw terminal mode for input capture)
    pub(super) fn enable_tty_input(&mut self) {
        if !self.tty_input_enabled {
            // Only enable raw mode if stdin is actually a terminal (not piped)
            if io::stdin().is_terminal() {
                let _ = terminal::enable_raw_mode();
                self.tty_input_enabled = true;
            }
        }
    }
    
    /// Disable TTY input mode
    pub(super) fn disable_tty_input(&mut self) {
        if self.tty_input_enabled {
            let _ = terminal::disable_raw_mode();
            self.tty_input_enabled = false;
        }
    }
    
    /// Poll stdin for input (handles both terminal and piped input)
    pub(super) fn poll_stdin(&mut self) {
        // Terminal input - use crossterm events if raw mode is enabled
        if self.tty_input_enabled {
            // Use crossterm to check for available input
            if event::poll(Duration::from_millis(0)).unwrap_or(false) {
                if let Ok(Event::Key(key_event)) = event::read() {
                    // Convert key event to ASCII character
                    match key_event.code {
                        KeyCode::Char(c) => {
                            // Push character to input buffer
                            self.input_buffer.push_back(c as u8);
                        },
                        KeyCode::Enter => {
                            // Push newline
                            self.input_buffer.push_back(b'\n');
                        },
                        KeyCode::Backspace => {
                            // Push backspace (ASCII 8)
                            self.input_buffer.push_back(8);
                        },
                        KeyCode::Tab => {
                            // Push tab
                            self.input_buffer.push_back(b'\t');
                        },
                        KeyCode::Esc => {
                            // Push escape (ASCII 27)
                            self.input_buffer.push_back(27);
                        },
                        _ => {
                            // Ignore other special keys for now
                        }
                    }
                }
            }
        }
        // For piped input, we rely on pre-populated input buffer via --input flag
        // Real-time piped input would require platform-specific non-blocking I/O
    }
    
    /// Enter TTY mode (for simple stdin/stdout with raw input)
    pub(super) fn enter_tty_mode(&mut self) {
        // Only enter raw mode if not in verbose/debug mode
        if self.verbose || self.debug_mode {
            return; // Keep normal mode for debugging
        }
        
        // Enable raw mode to capture input immediately
        let _ = terminal::enable_raw_mode();
        self.terminal_raw_mode = true;
    }
    
    /// Exit TTY mode
    pub(super) fn exit_tty_mode(&mut self) {
        if !self.terminal_raw_mode {
            return;
        }
        
        // Restore terminal
        let _ = terminal::disable_raw_mode();
        self.terminal_raw_mode = false;
    }
    
    /// Enter TEXT40 terminal mode
    pub(super) fn enter_text40_mode(&mut self) {
        // Only enter raw mode if not in verbose/debug mode
        if self.verbose || self.debug_mode {
            return; // Keep normal mode for debugging
        }
        
        // Save current screen and enter alternate screen
        let _ = io::stderr().execute(terminal::EnterAlternateScreen);
        let _ = io::stderr().execute(cursor::Hide);
        let _ = terminal::enable_raw_mode();
        
        self.terminal_raw_mode = true;
        self.terminal_saved_screen = true;
        
        // Clear keyboard state when entering TEXT40 mode
        self.clear_keyboard_state();
        
        // Clear the screen
        let _ = io::stderr().execute(terminal::Clear(ClearType::All));
        let _ = io::stderr().execute(cursor::MoveTo(0, 0));
        let _ = io::stderr().flush();
    }
    
    /// Exit TEXT40 terminal mode
    pub(super) fn exit_text40_mode(&mut self) {
        if !self.terminal_raw_mode {
            return;
        }
        
        // Restore terminal
        let _ = terminal::disable_raw_mode();
        let _ = io::stderr().execute(cursor::Show);
        let _ = io::stderr().execute(terminal::LeaveAlternateScreen);
        let _ = io::stderr().flush();
        
        self.terminal_raw_mode = false;
        self.terminal_saved_screen = false;
    }
}

/// Install a panic hook to ensure terminal cleanup
pub fn install_terminal_cleanup_hook() {
    use std::panic;
    
    let original_hook = panic::take_hook();
    panic::set_hook(Box::new(move |panic_info| {
        // Try to restore terminal before panicking
        let _ = terminal::disable_raw_mode();
        let _ = io::stderr().execute(cursor::Show);
        let _ = io::stderr().execute(terminal::LeaveAlternateScreen);
        let _ = io::stderr().execute(ResetColor);
        
        // Call the original panic hook
        original_hook(panic_info);
    }));
}