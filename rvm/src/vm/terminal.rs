use super::state::KeyboardState;
use super::VM;
use crate::constants::*;
use crossterm::{
    cursor,
    event::{self, Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers},
    style::ResetColor,
    terminal::{self, ClearType},
    ExecutableCommand,
};
use std::io::{self, IsTerminal, Write};
use std::time::Duration;

impl VM {
    /// Clear keyboard state flags
    pub(super) fn clear_keyboard_state(&mut self) {
        self.keyboard_state = KeyboardState::default();
    }

    /// Poll independently of guest MMIO reads, so a busy or broken guest cannot
    /// swallow the host stop key while raw mode disables host signals.
    pub fn poll_host_terminal(&mut self) -> bool {
        if self.host_input_enabled && !self.debug_mode && !self.verbose
            && (self.tty_input_enabled || self.terminal_raw_mode)
        {
            self.poll_terminal_event();
        }
        self.terminal_quit_requested
    }

    fn poll_terminal_event(&mut self) {
        if event::poll(Duration::ZERO).unwrap_or(false) {
            if let Ok(Event::Key(key)) = event::read() {
                self.accept_terminal_key(key);
            }
        }
    }

    fn accept_terminal_key(&mut self, key: KeyEvent) {
        if key.kind == KeyEventKind::Release {
            return;
        }
        if key.modifiers.contains(KeyModifiers::CONTROL)
            && ((matches!(key.code, KeyCode::Char('c' | 'C'))
                && (!self.guest_ctrl_c || self.display_mode == DISP_TEXT40))
                || (self.guest_ctrl_c && key.code == KeyCode::Char(']')))
        {
            self.terminal_quit_requested = true;
            return;
        }
        if self.display_mode == DISP_TEXT40 {
            self.keyboard_state = KeyboardState::default();
            match key.code {
                KeyCode::Up => self.keyboard_state.up = true,
                KeyCode::Down => self.keyboard_state.down = true,
                KeyCode::Left => self.keyboard_state.left = true,
                KeyCode::Right => self.keyboard_state.right = true,
                KeyCode::Char('z' | 'Z') => self.keyboard_state.z = true,
                KeyCode::Char('x' | 'X') => self.keyboard_state.x = true,
                _ => {}
            }
        } else {
            // Crossterm also represents a quickly typed Esc followed by a
            // character as Alt+character. Preserve that escape for terminal
            // applications which use Esc to leave an input mode.
            if key.modifiers.contains(KeyModifiers::ALT)
                && matches!(key.code, KeyCode::Char(_) | KeyCode::Enter
                    | KeyCode::Backspace | KeyCode::Tab | KeyCode::Esc)
            {
                self.input_buffer.push_back(27);
            }
            match key.code {
                KeyCode::Char(c) if key.modifiers.contains(KeyModifiers::CONTROL) && c.is_ascii() => {
                    // Crossterm reports e.g. LF as Ctrl-J and EOF as Ctrl-D.
                    // Preserve their control codes for guest terminal drivers.
                    let byte = if c == '?' { 127 } else { c.to_ascii_uppercase() as u8 & 31 };
                    self.input_buffer.push_back(byte);
                }
                KeyCode::Char(c) => self.input_buffer.push_back(c as u8),
                KeyCode::Enter => self.input_buffer.push_back(b'\n'),
                KeyCode::Backspace => self.input_buffer.push_back(8),
                KeyCode::Tab => self.input_buffer.push_back(b'\t'),
                KeyCode::Esc => self.input_buffer.push_back(27),
                _ => {}
            }
        }
    }

    pub(super) fn poll_keyboard(&mut self) {
        if self.display_mode == DISP_TEXT40 && self.terminal_raw_mode {
            self.poll_terminal_event();
        }
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

    /// Piped input is supplied through --input; live terminal events also
    /// service Ctrl-C through the same path as the runner's host polling.
    pub(super) fn poll_stdin(&mut self) {
        if self.tty_input_enabled {
            self.poll_terminal_event();
        }
    }

    /// Enter TTY mode (for simple stdin/stdout with raw input)
    pub(super) fn enter_tty_mode(&mut self) {
        // Only enter raw mode if not in verbose/debug mode
        if self.verbose || self.debug_mode || !self.host_input_enabled {
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
        if self.verbose || self.debug_mode || !self.host_input_enabled {
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn control_c_stops_the_host_without_becoming_guest_input() {
        let mut vm = VM::with_memory_size(64, 4096);
        vm.host_input_enabled = false;
        for mode in [DISP_TTY, DISP_TEXT40] {
            vm.display_mode = mode;
            vm.terminal_quit_requested = false;
            vm.accept_terminal_key(KeyEvent::new(KeyCode::Char('c'), KeyModifiers::CONTROL));
            assert!(vm.poll_host_terminal());
            assert!(vm.input_buffer.is_empty());
        }
        vm.reset();
        assert!(!vm.poll_host_terminal());
    }

    #[test]
    fn ordinary_terminal_input_and_text40_keys_still_reach_the_guest() {
        let mut vm = VM::with_memory_size(64, 4096);
        vm.accept_terminal_key(KeyEvent::new(KeyCode::Char('c'), KeyModifiers::NONE));
        vm.accept_terminal_key(KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE));
        assert_eq!(vm.input_buffer.iter().copied().collect::<Vec<_>>(), b"c\n");
        vm.display_mode = DISP_TEXT40;
        vm.accept_terminal_key(KeyEvent::new(KeyCode::Up, KeyModifiers::NONE));
        assert!(vm.keyboard_state.up);
        assert!(!vm.terminal_quit_requested);
    }

    #[test]
    fn tty_control_keys_keep_their_ascii_codes() {
        let mut vm = VM::with_memory_size(64, 4096);
        for c in ['j', 'd', 'u', '?'] {
            vm.accept_terminal_key(KeyEvent::new(KeyCode::Char(c), KeyModifiers::CONTROL));
        }
        assert_eq!(vm.input_buffer.iter().copied().collect::<Vec<_>>(), [10, 4, 21, 127]);
        assert!(!vm.terminal_quit_requested);
    }

    #[test]
    fn guest_interrupt_does_not_stop_the_vm_and_has_a_separate_host_escape() {
        let mut vm = VM::with_memory_size(64, 4096);
        vm.host_input_enabled = false;
        vm.guest_ctrl_c = true;
        vm.display_mode = DISP_TTY;
        vm.accept_terminal_key(KeyEvent::new(KeyCode::Char('c'), KeyModifiers::CONTROL));
        assert_eq!(vm.input_buffer.iter().copied().collect::<Vec<_>>(), [3]);
        assert!(!vm.poll_host_terminal());
        vm.accept_terminal_key(KeyEvent::new(KeyCode::Char(']'), KeyModifiers::CONTROL));
        assert!(vm.poll_host_terminal());
        assert_eq!(vm.input_buffer.iter().copied().collect::<Vec<_>>(), [3]);
    }

    #[test]
    fn tty_alt_keys_preserve_the_escape_prefix() {
        let mut vm = VM::with_memory_size(64, 4096);
        vm.accept_terminal_key(KeyEvent::new(KeyCode::Char(':'), KeyModifiers::ALT));
        vm.accept_terminal_key(KeyEvent::new(
            KeyCode::Char('f'), KeyModifiers::ALT | KeyModifiers::CONTROL));
        vm.accept_terminal_key(KeyEvent::new(KeyCode::Esc, KeyModifiers::ALT));
        assert_eq!(vm.input_buffer.iter().copied().collect::<Vec<_>>(),
                   [27, b':', 27, 6, 27, 27]);
        assert!(!vm.terminal_quit_requested);
    }
}
