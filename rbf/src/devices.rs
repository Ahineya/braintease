//! Host devices for the Brainfuck CPU. This module does not decode or execute
//! Ripple instructions: the CPU calls it at explicit RAM read/write boundaries.
use rvm::{
    constants::*,
    devices::{DeviceState, GuestMemory, KeyTransition},
    display_rgb565::RGB565Display,
    vm::Storage,
};
use std::collections::VecDeque;

pub struct BfDevices {
    pub devices: DeviceState,
    pub display: RGB565Display,
    pub presented_frames: u64,
    pub host_input_enabled: bool,
    bank_size: u16,
    rng_state: u32,
    display_mode: u16,
    display_resolution: u16,
    display_enabled: bool,
    input: VecDeque<u8>,
    output: Vec<u8>,
    storage: Option<Storage>,
}

impl BfDevices {
    pub fn new(
        bank_size: u16,
        tick_hz: u16,
        seed: u32,
        storage: Option<Storage>,
    ) -> Result<Self, String> {
        if bank_size < 32 {
            return Err("Brainfuck host banks must hold all 32 MMIO registers".into());
        }
        Ok(Self {
            devices: DeviceState::new(tick_hz)?,
            display: RGB565Display::new(),
            presented_frames: 0,
            host_input_enabled: true,
            bank_size,
            rng_state: seed,
            display_mode: DISP_OFF,
            display_resolution: 0,
            display_enabled: false,
            input: VecDeque::new(),
            output: Vec::new(),
            storage,
        })
    }

    pub fn bank_size(&self) -> u16 {
        self.bank_size
    }
    pub fn display_mode(&self) -> u16 {
        self.display_mode
    }
    pub fn push_input(&mut self, bytes: &[u8]) {
        self.input.extend(bytes);
    }
    pub fn take_output(&mut self) -> Vec<u8> {
        std::mem::take(&mut self.output)
    }
    pub fn window_closed(&self) -> bool {
        self.display.is_window_closed()
    }
    pub fn flush_storage(&mut self) -> Result<(), String> {
        if let Some(storage) = &mut self.storage {
            storage
                .flush_checked()
                .map_err(|e| format!("Storage flush: {e}"))?;
        }
        Ok(())
    }

    fn check_address<M: GuestMemory + ?Sized>(
        &self,
        address: usize,
        memory: &M,
    ) -> Result<(), String> {
        if address >= memory.len() {
            return Err(format!(
                "Guest RAM address {address} exceeds {} cells",
                memory.len()
            ));
        }
        Ok(())
    }

    /// Address is an absolute logical RAM index, not a physical BF tape index.
    /// The caller writes this returned value into the RAM lane before BF LOAD
    /// copies it. Reading a device is performed exactly once per LOAD hook.
    pub fn read_mmio<M: GuestMemory + ?Sized>(
        &mut self,
        address: usize,
        memory: &mut M,
    ) -> Result<u16, String> {
        self.check_address(address, memory)?;
        if address >= 32 {
            if address < usize::from(self.bank_size) && self.display_mode == DISP_RGB565 {
                if let Some(value) = self.display.read_memory(address) {
                    return Ok(value);
                }
            }
            return Ok(memory.read(address));
        }
        let value = match address {
            HDR_TTY_OUT | HDR_DISP_FLUSH | HDR_STORE_BLOCK | HDR_STORE_ADDR => 0,
            // Device operations complete synchronously at the host boundary.
            HDR_TTY_STATUS => TTY_READY,
            HDR_TTY_IN_POP => u16::from(self.input.pop_front().unwrap_or(0)),
            HDR_TTY_IN_STATUS => {
                if self.input.is_empty() {
                    0
                } else {
                    TTY_HAS_BYTE
                }
            }
            HDR_RNG => {
                self.rng_state = self
                    .rng_state
                    .wrapping_mul(1664525)
                    .wrapping_add(1013904223);
                (self.rng_state >> 16) as u16
            }
            HDR_RNG_SEED => self.rng_state as u16,
            HDR_DISP_MODE => self.display_mode,
            HDR_DISP_STATUS => DISP_READY | DISP_FLUSH_DONE,
            HDR_DISP_CTL => {
                if self.display_enabled {
                    DISP_ENABLE
                } else {
                    0
                }
            }
            HDR_KEY_UP..=HDR_KEY_X => {
                let key = [82, 81, 80, 79, 29, 27][address - HDR_KEY_UP];
                let down = if self.display_mode == DISP_RGB565_EXT || !self.host_input_enabled {
                    self.devices.held(key)
                } else if self.display_mode == DISP_RGB565 {
                    self.display.get_state().lock().unwrap().keys_held[usize::from(key)]
                } else {
                    false
                };
                u16::from(down)
            }
            HDR_DISP_RESOLUTION => self.display_resolution,
            HDR_STORE_DATA => {
                if let Some(storage) = &mut self.storage {
                    let value = storage
                        .read_word_checked()
                        .map_err(|e| format!("Storage read: {e}"))?;
                    memory.write(HDR_STORE_BLOCK, storage.get_block());
                    memory.write(HDR_STORE_ADDR, storage.get_addr());
                    value
                } else {
                    0
                }
            }
            HDR_STORE_CTL => self.storage.as_ref().map_or(0, Storage::get_control),
            // The BF CPU has no interrupt dispatcher. Zero denotes no pending
            // or configured interrupts; nonzero writes are rejected below.
            HDR_IRQ_STATUS..=HDR_IRQ_CAUSE => 0,
            27..=31 => self
                .devices
                .read_register(address)
                .expect("mailbox register"),
            _ => unreachable!("all MMIO addresses are handled"),
        };
        memory.write(address, value);
        Ok(value)
    }

    /// Called once after BF STORE has completed its RAM write. In legacy RGB
    /// mode the display owns the front/back mapping, including ignored writes
    /// to the front buffer. Ordinary RAM remains owned by the BF tape.
    pub fn write_mmio<M: GuestMemory + ?Sized>(
        &mut self,
        address: usize,
        value: u16,
        memory: &mut M,
    ) -> Result<(), String> {
        self.check_address(address, memory)?;
        if address >= 32 {
            if address < usize::from(self.bank_size)
                && self.display_mode == DISP_RGB565
                && self.display.write_memory(address, value)
            {
                // Keep the tape's shadow consistent after an ignored front write.
                memory.write(address, self.display.read_memory(address).unwrap());
                return Ok(());
            }
            memory.write(address, value);
            return Ok(());
        }
        match address {
            HDR_TTY_OUT => self.output.push(value as u8),
            HDR_RNG_SEED => self.rng_state = (self.rng_state & 0xffff0000) | u32::from(value),
            HDR_DISP_MODE => self.change_display_mode(value)?,
            HDR_DISP_CTL => {
                if value & DISP_ENABLE != 0 {
                    self.display_enabled = true;
                }
                if value & DISP_CLEAR != 0 {
                    for index in TEXT40_BASE_WORD..(TEXT40_LAST_WORD + 1).min(memory.len()) {
                        memory.write(index, 0);
                    }
                }
            }
            HDR_DISP_FLUSH => {
                if value != 0 && self.display_mode == DISP_RGB565 && self.display.flush() {
                    self.presented_frames += 1;
                }
            }
            HDR_DISP_RESOLUTION => self.display_resolution = value,
            HDR_STORE_BLOCK => {
                if let Some(storage) = &mut self.storage {
                    storage.set_block(value);
                }
            }
            HDR_STORE_ADDR => {
                if let Some(storage) = &mut self.storage {
                    storage.set_addr(value);
                }
            }
            HDR_STORE_DATA => {
                if let Some(storage) = &mut self.storage {
                    storage
                        .write_word_checked(value)
                        .map_err(|e| format!("Storage write: {e}"))?;
                }
            }
            HDR_STORE_CTL => {
                if let Some(storage) = &mut self.storage {
                    storage
                        .set_control_checked(value)
                        .map_err(|e| format!("Storage control: {e}"))?;
                }
            }
            HDR_IRQ_STATUS..=HDR_IRQ_CAUSE => {
                if value != 0 {
                    return Err("Interrupts are unsupported by the Brainfuck CPU".into());
                }
            }
            27..=31 => {
                if let Some(frame) = self.devices.write_register_with_memory(
                    address,
                    value,
                    memory,
                    self.bank_size,
                    self.display_mode,
                ) {
                    self.display
                        .present(frame.width, frame.height, frame.pixels)?;
                    self.presented_frames += 1;
                }
            }
            // Read-only status, input and RNG registers ignore stores.
            _ => {}
        }
        Ok(())
    }

    fn change_display_mode(&mut self, mode: u16) -> Result<(), String> {
        if mode == self.display_mode {
            return Ok(());
        }
        match mode {
            DISP_OFF | DISP_TTY | DISP_RGB565_EXT => self.display.disable(),
            DISP_RGB565 => self.display.init(
                (self.display_resolution >> 8) as u8,
                self.display_resolution as u8,
                usize::from(self.bank_size),
            )?,
            DISP_TEXT40 => {
                return Err("TEXT40 terminal rendering is unsupported by the Brainfuck host".into())
            }
            _ => return Err(format!("Unsupported display mode {mode}")),
        }
        self.display_mode = mode;
        Ok(())
    }

    /// Complete only a requested tick. Window input is latched at this boundary,
    /// with the same overflow recovery and ordering as the Rust VM.
    pub fn complete_tick<M: GuestMemory + ?Sized>(
        &mut self,
        memory: &mut M,
        mut events: Vec<KeyTransition>,
    ) -> Result<(), String> {
        if !self.devices.has_pending_tick() {
            return Ok(());
        }
        if self.host_input_enabled {
            let shared = self.display.get_state();
            let mut state = shared.lock().unwrap();
            if state.key_overflow {
                self.devices.mark_input_overflow();
                state.pending_keys.clear();
                for (key, down) in state.keys_held.iter().copied().enumerate() {
                    if self.devices.held(key as u16) != down {
                        events.push(KeyTransition {
                            key: key as u16,
                            down,
                        });
                    }
                }
                state.key_overflow = false;
            } else {
                events.extend(
                    std::mem::take(&mut state.pending_keys)
                        .into_iter()
                        .map(|(key, down)| KeyTransition { key, down }),
                );
            }
        }
        self.devices
            .complete_tick_with_memory(memory, self.bank_size, &events)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rvm::devices::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    fn host() -> BfDevices {
        BfDevices::new(64, 35, 1, None).unwrap()
    }
    fn store(host: &mut BfDevices, memory: &mut Vec<u16>, address: usize, value: u16) {
        memory[address] = value; // The BF STORE runs before the host hook.
        host.write_mmio(address, value, memory).unwrap();
    }
    fn command(host: &mut BfDevices, memory: &mut Vec<u16>, command: u16, length: u16) {
        memory[63] = ABI_VERSION;
        memory[64] = length;
        store(host, memory, REGISTER_DESCRIPTOR_OFFSET, 63);
        store(host, memory, REGISTER_COMMAND, command);
    }

    #[test]
    fn tty_rng_and_unsupported_configuration_have_explicit_semantics() {
        let mut host = host();
        let mut memory = vec![0; 256];
        store(&mut host, &mut memory, HDR_TTY_OUT, 0x0141);
        store(&mut host, &mut memory, HDR_TTY_OUT, 0x0141);
        assert_eq!(host.take_output(), b"AA");
        assert!(host.take_output().is_empty());
        assert_eq!(
            host.read_mmio(HDR_TTY_STATUS, &mut memory).unwrap(),
            TTY_READY
        );
        host.push_input(&[0, 255, 65]);
        assert_eq!(
            host.read_mmio(HDR_TTY_IN_STATUS, &mut memory).unwrap(),
            TTY_HAS_BYTE
        );
        assert_eq!(host.read_mmio(HDR_TTY_IN_POP, &mut memory).unwrap(), 0);
        assert_eq!(host.read_mmio(HDR_TTY_IN_POP, &mut memory).unwrap(), 255);
        assert_eq!(host.read_mmio(HDR_TTY_IN_POP, &mut memory).unwrap(), 65);
        assert_eq!(host.read_mmio(HDR_TTY_IN_STATUS, &mut memory).unwrap(), 0);
        store(&mut host, &mut memory, HDR_RNG, 99); // Read-only.
        assert_eq!(host.read_mmio(HDR_RNG, &mut memory).unwrap(), 0x3c88);
        assert_eq!(host.read_mmio(HDR_RNG, &mut memory).unwrap(), 0x5e88);
        store(&mut host, &mut memory, HDR_RNG_SEED, 7);
        assert_eq!(host.rng_state, 0x5e880007);
        for address in HDR_IRQ_STATUS..=HDR_IRQ_CAUSE {
            assert_eq!(host.read_mmio(address, &mut memory).unwrap(), 0);
            assert!(host.write_mmio(address, 0, &mut memory).is_ok());
            assert!(host.write_mmio(address, 1, &mut memory).is_err());
        }
        assert!(host
            .write_mmio(HDR_DISP_MODE, DISP_TEXT40, &mut memory)
            .is_err());
        assert_eq!(host.display_mode(), DISP_OFF);
        assert!(host.read_mmio(256, &mut memory).is_err());
        assert!(host.write_mmio(256, 1, &mut memory).is_err());
        assert!(BfDevices::new(31, 35, 1, None).is_err());
    }

    #[test]
    fn legacy_frames_repeat_flush_and_do_not_claim_other_banks() {
        let mut host = host();
        let mut memory = vec![0; 256];
        store(&mut host, &mut memory, HDR_DISP_RESOLUTION, 0x0201);
        store(&mut host, &mut memory, HDR_DISP_MODE, DISP_RGB565);
        store(&mut host, &mut memory, 34, 0xf800);
        store(&mut host, &mut memory, 35, 0x07e0);
        store(&mut host, &mut memory, 32, 0xffff); // Ignore writes to front.
        assert_eq!(host.read_mmio(32, &mut memory).unwrap(), 0);
        store(&mut host, &mut memory, HDR_DISP_FLUSH, 1);
        let first = host.display.snapshot().unwrap();
        assert_eq!(first.pixels, [0xf800, 0x07e0]);
        assert_eq!(host.presented_frames, 1);
        assert_eq!(host.read_mmio(34, &mut memory).unwrap(), 0);
        store(&mut host, &mut memory, HDR_DISP_FLUSH, 1);
        assert_eq!(host.presented_frames, 2);
        assert_eq!(host.display.snapshot().unwrap().pixels, [0, 0]);
        assert_eq!(first.pixels, [0xf800, 0x07e0]);
        store(&mut host, &mut memory, 64 + 34, 0xbeef);
        assert_eq!(host.read_mmio(64 + 34, &mut memory).unwrap(), 0xbeef);
        host.display.get_state().lock().unwrap().set_key(82, true);
        assert_eq!(host.read_mmio(HDR_KEY_UP, &mut memory).unwrap(), 1);
        host.host_input_enabled = false;
        assert_eq!(host.read_mmio(HDR_KEY_UP, &mut memory).unwrap(), 0);
        store(&mut host, &mut memory, HDR_DISP_MODE, DISP_RGB565_EXT);
        assert!(host.display.snapshot().is_none());
        assert!(!host.window_closed());
    }

    #[test]
    fn extended_snapshot_and_tick_input_cross_banks_without_early_visibility() {
        let mut host = host();
        let mut memory = vec![0; 256];
        store(&mut host, &mut memory, HDR_DISP_MODE, DISP_RGB565_EXT);
        memory[65..71].copy_from_slice(&[3, 2, 5, 1, 62, 0]);
        memory[126..134].copy_from_slice(&[1, 2, 3, 99, 99, 4, 5, 6]);
        command(&mut host, &mut memory, COMMAND_PRESENT, 8);
        assert_eq!(host.devices.status(), STATUS_OK);
        let first = host.display.snapshot().unwrap();
        assert_eq!(first.pixels, [1, 2, 3, 4, 5, 6]);
        memory[126] = 9;
        store(&mut host, &mut memory, REGISTER_COMMAND, COMMAND_PRESENT);
        assert_eq!(host.presented_frames, 2);
        assert_eq!(first.pixels[0], 1);
        assert_eq!(host.display.snapshot().unwrap().pixels[0], 9);
        memory[70] = 1; // Reserved descriptor word must be zero.
        store(&mut host, &mut memory, REGISTER_COMMAND, COMMAND_PRESENT);
        assert_eq!(host.devices.status(), STATUS_BAD_DESCRIPTOR);
        assert_eq!(host.presented_frames, 2);

        host.devices
            .set_script(
                InputScript::from_json(r#"[{"tick":1,"key":"ENTER","down":true}]"#).unwrap(),
            )
            .unwrap();
        host.display.get_state().lock().unwrap().set_key(82, true);
        host.complete_tick(&mut memory, vec![]).unwrap();
        assert_eq!(host.devices.tick(), 0);
        assert_eq!(host.read_mmio(HDR_KEY_UP, &mut memory).unwrap(), 0);
        command(&mut host, &mut memory, COMMAND_NEXT_TICK, 8);
        assert_eq!(
            host.read_mmio(REGISTER_STATUS, &mut memory).unwrap(),
            STATUS_PENDING
        );
        host.complete_tick(&mut memory, vec![KeyTransition { key: 4, down: true }])
            .unwrap();
        assert_eq!(host.devices.tick(), 1);
        assert_eq!(host.read_mmio(HDR_KEY_UP, &mut memory).unwrap(), 1);
        for key in [40, 4, 82] {
            command(&mut host, &mut memory, COMMAND_INPUT_POP, 8);
            assert_eq!(&memory[65..70], &[1, key, 1, 1, 0]);
        }
        command(&mut host, &mut memory, COMMAND_GET_TIME, 8);
        assert_eq!(&memory[65..71], &[1, 0, 28, 0, 35, 0]);
        assert_eq!(host.devices.tick(), 1);
    }

    #[test]
    fn host_input_overflow_resynchronizes_and_deterministic_mode_ignores_window() {
        let mut host = host();
        let mut memory = vec![0; 256];
        {
            let shared = host.display.get_state();
            let mut state = shared.lock().unwrap();
            for index in 0..4097 {
                state.set_key(4, index % 2 == 0);
            }
            assert!(state.key_overflow);
        }
        host.host_input_enabled = false;
        command(&mut host, &mut memory, COMMAND_NEXT_TICK, 8);
        host.complete_tick(&mut memory, vec![]).unwrap();
        assert!(!host.devices.held(4));
        assert!(!host.devices.overflow_seen());
        host.host_input_enabled = true;
        command(&mut host, &mut memory, COMMAND_NEXT_TICK, 8);
        host.complete_tick(&mut memory, vec![]).unwrap();
        assert!(host.devices.held(4));
        assert!(host.devices.overflow_seen());
        assert_eq!(host.devices.status(), STATUS_INPUT_OVERFLOW);
        assert!(!host.display.get_state().lock().unwrap().key_overflow);
        command(&mut host, &mut memory, COMMAND_INPUT_STATE, 19);
        assert_eq!(memory[65], 1);
        assert_eq!(memory[66] & (1 << 4), 1 << 4);
        assert!(!host.devices.input_overflowed());
    }

    #[test]
    fn storage_word_ports_preserve_partial_disk_and_commit_changes() {
        static NEXT_DISK: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!(
            "rbf-device-{}-{}.img",
            std::process::id(),
            NEXT_DISK.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::write(&path, [0x34, 0x12, 0x56]).unwrap();
        let storage = Storage::with_path(path.clone()).unwrap();
        let mut host = BfDevices::new(64, 35, 1, Some(storage)).unwrap();
        let mut memory = vec![0; 256];
        assert_eq!(host.read_mmio(HDR_STORE_DATA, &mut memory).unwrap(), 0x1234);
        assert_eq!(memory[HDR_STORE_ADDR], 1);
        assert_eq!(host.read_mmio(HDR_STORE_DATA, &mut memory).unwrap(), 0x0056);
        store(&mut host, &mut memory, HDR_STORE_ADDR, 1);
        store(&mut host, &mut memory, HDR_STORE_DATA, 0xabcd);
        assert_eq!(
            host.read_mmio(HDR_STORE_CTL, &mut memory).unwrap(),
            STORE_DIRTY
        );
        store(&mut host, &mut memory, HDR_STORE_CTL, STORE_COMMIT);
        assert_eq!(host.read_mmio(HDR_STORE_CTL, &mut memory).unwrap(), 0);
        host.flush_storage().unwrap();
        drop(host);
        let bytes = std::fs::read(&path).unwrap();
        assert_eq!(&bytes[..4], &[0x34, 0x12, 0xcd, 0xab]);
        std::fs::remove_file(path).unwrap();
    }
}
