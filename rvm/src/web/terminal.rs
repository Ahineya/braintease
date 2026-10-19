//! Browser input/output belongs to the worker bridge, not an OS terminal.
use super::VM;
impl VM {
    pub(super) fn clear_keyboard_state(&mut self) {
        self.keyboard_state = Default::default();
    }
    pub fn poll_host_terminal(&mut self) -> bool {
        false
    }
    pub(super) fn poll_keyboard(&mut self) {}
    pub(super) fn enable_tty_input(&mut self) {}
    pub(super) fn disable_tty_input(&mut self) {}
    pub(super) fn poll_stdin(&mut self) {}
    pub(super) fn enter_tty_mode(&mut self) {}
    pub(super) fn exit_tty_mode(&mut self) {}
    pub(super) fn enter_text40_mode(&mut self) {}
    pub(super) fn exit_text40_mode(&mut self) {}
}
pub fn install_terminal_cleanup_hook() {}
