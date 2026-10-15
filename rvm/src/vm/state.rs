/// VM execution states
#[derive(Debug)]
pub enum VMState {
    Setup,
    Running,
    Halted,
    Breakpoint, // Hit a BRK instruction in debug mode
    Error(String),
}

/// Keyboard input state tracking
#[derive(Debug, Default, Clone, Copy)]
pub struct KeyboardState {
    pub up: bool,
    pub down: bool,
    pub left: bool,
    pub right: bool,
    pub z: bool,
    pub x: bool,
    pub last_read_counter: u32, // Counter to track when keys were last read
}
