//! Central configuration and constants for the Ripple VM

#![allow(dead_code)]

// Default VM configuration
pub const DEFAULT_BANK_SIZE: u16 = 65535;
pub const OP_HALT: u8 = 0x00;
pub const OP_NOP: u8 = 0x42;
pub const DEFAULT_MEMORY_SIZE: usize = 65536 * 65536; // 64K words in 64K banks

// Memory-mapped I/O header addresses (bank 0, words 0..31)
pub const HDR_TTY_OUT: usize       = 0;  // Write: low8 → stdout
pub const HDR_TTY_STATUS: usize    = 1;  // Read: bit0=ready
pub const HDR_TTY_IN_POP: usize    = 2;  // Read: pop next input byte
pub const HDR_TTY_IN_STATUS: usize = 3;  // Read: bit0=has_byte
pub const HDR_RNG: usize           = 4;  // Read: next PRNG value
pub const HDR_RNG_SEED: usize      = 5;  // R/W: RNG seed (low 16 bits)
pub const HDR_DISP_MODE: usize     = 6;  // R/W: 0=OFF, 1=TTY, 2=TEXT40
pub const HDR_DISP_STATUS: usize   = 7;  // Read: bit0=ready, bit1=flush_done
pub const HDR_DISP_CTL: usize      = 8;  // R/W: bit0=ENABLE, bit1=CLEAR
pub const HDR_DISP_FLUSH: usize    = 9;  // Write: trigger display flush

// Keyboard input flags (bank 0, words 10..15)
pub const HDR_KEY_UP: usize        = 10; // Read: bit0=pressed (arrow up)
pub const HDR_KEY_DOWN: usize      = 11; // Read: bit0=pressed (arrow down)
pub const HDR_KEY_LEFT: usize      = 12; // Read: bit0=pressed (arrow left)
pub const HDR_KEY_RIGHT: usize     = 13; // Read: bit0=pressed (arrow right)
pub const HDR_KEY_Z: usize         = 14; // Read: bit0=pressed (Z key)
pub const HDR_KEY_X: usize         = 15; // Read: bit0=pressed (X key)

// Display resolution for RGB565 mode (bank 0, word 16)
pub const HDR_DISP_RESOLUTION: usize = 16; // R/W: hi8=width, lo8=height (for RGB565 mode)

// Storage device registers (bank 0, words 17..20)
pub const HDR_STORE_BLOCK: usize = 17; // Write: Select current block (0-65535)
pub const HDR_STORE_ADDR: usize = 18;  // Write: Select word address within block (0-65535)
pub const HDR_STORE_DATA: usize = 19;  // R/W: Data register for current (block, addr)
pub const HDR_STORE_CTL: usize = 20;   // R/W: Control register (busy/dirty/commit bits)

// Interrupt controller (bank 0, words 21..26)
pub const HDR_IRQ_STATUS: usize = 21;       // R/W: pending bits (write ORs into pending)
pub const HDR_IRQ_ENABLE: usize = 22;       // R/W: enable mask
pub const HDR_IRQ_BUSY: usize = 23;         // R: in-service (0/1); W: nonzero acknowledges
pub const HDR_IRQ_VECTOR_BANK: usize = 24;  // R/W: handler PCB
pub const HDR_IRQ_VECTOR_OFF: usize = 25;   // R/W: handler PC
pub const HDR_IRQ_CAUSE: usize = 26;        // R: 0=none, else 1 + bit index of in-service IRQ

// Software interrupt source (STATUS/ENABLE bit 0)
pub const IRQ_SW: u16 = 1 << 0;

// Storage control bits
pub const STORE_BUSY: u16 = 1 << 0;        // bit0: VM is processing storage operation
pub const STORE_DIRTY: u16 = 1 << 1;       // bit1: Current block has uncommitted writes
pub const STORE_COMMIT: u16 = 1 << 2;      // bit2: Trigger commit of current block
pub const STORE_COMMIT_ALL: u16 = 1 << 3;  // bit3: Trigger commit of all dirty blocks

// Words 27..31 are reserved for future use

// TEXT40 display constants
pub const TEXT40_BASE_WORD: usize  = 32;         // Start of VRAM
pub const TEXT40_WORDS: usize      = 40 * 25;    // 1000 words (40x25 cells)
pub const TEXT40_LAST_WORD: usize  = TEXT40_BASE_WORD + TEXT40_WORDS - 1; // 1031

// TTY status bits
pub const TTY_READY: u16 = 1 << 0;      // bit0
pub const TTY_HAS_BYTE: u16 = 1 << 0;   // bit0

// Display mode values
pub const DISP_OFF: u16    = 0;
pub const DISP_TTY: u16    = 1;
pub const DISP_TEXT40: u16 = 2;
pub const DISP_RGB565: u16 = 3;

// Display status bits
pub const DISP_READY: u16      = 1 << 0;  // bit0
pub const DISP_FLUSH_DONE: u16 = 1 << 1;  // bit1

// Display control bits
pub const DISP_ENABLE: u16 = 1 << 0;    // bit0
pub const DISP_CLEAR: u16  = 1 << 1;    // bit1 (edge-triggered)

// Legacy MMIO addresses (kept for compatibility)
pub const MMIO_OUT: usize = HDR_TTY_OUT;       // Output register
pub const MMIO_OUT_FLAG: usize = HDR_TTY_STATUS;  // Output ready flag

// Memory layout
pub const DATA_SECTION_OFFSET: usize = TEXT40_LAST_WORD + 1; // Data section starts after VRAM (word 1032)

// Instruction encoding
pub const INSTRUCTION_SIZE: usize = 8; // Bytes per instruction

// Binary format magic numbers
pub const MAGIC_RLINK: &[u8] = b"RLINK";

// Debug output configuration
pub const DEBUG_MEMORY_DISPLAY_WORDS: usize = 32; // Number of memory words to display in debug dumps
pub const DEBUG_MEMORY_WORDS_PER_LINE: usize = 8; // Words per line in memory dumps

// VM limits
pub const MAX_REGISTERS: usize = 32;
pub const MIN_MEMORY_SIZE: usize = 256; // Minimum reasonable memory size

// Output flags
pub const OUTPUT_READY: u16 = 1;
pub const OUTPUT_BUSY: u16 = 0;

// Theme Colors - PICO-8 Color Palette RGB values
pub const THEME_COLORS: [(u8, u8, u8); 16] = [
    (0, 0, 0),          // 0: Black
    (29, 43, 83),       // 1: Dark Blue
    (126, 37, 83),      // 2: Dark Purple
    (0, 135, 81),       // 3: Dark Green
    (171, 82, 54),      // 4: Brown
    (95, 87, 79),       // 5: Dark Gray
    (194, 195, 199),    // 6: Light Gray
    (255, 241, 232),    // 7: White
    (255, 0, 77),       // 8: Red
    (255, 163, 0),      // 9: Orange
    (255, 236, 39),     // 10: Yellow
    (0, 228, 54),       // 11: Green
    (41, 173, 255),     // 12: Blue
    (131, 118, 156),    // 13: Indigo
    (255, 119, 168),    // 14: Pink
    (255, 204, 170),    // 15: Peach
];