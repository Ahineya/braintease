//! Host devices for the Brainfuck CPU. This module does not decode or execute
//! Ripple instructions: the CPU calls it at explicit RAM read/write boundaries.
use rvm::{
    constants::*,
    devices::{DeviceState, GuestMemory, KeyTransition},
    display_rgb565::{RGB565Checkpoint, RGB565Display},
    display_text40::{Text40Checkpoint, Text40Display},
    vm::{Storage, StorageCheckpoint},
};
use serde::{Deserialize, Serialize};
use std::collections::VecDeque;

pub struct BfDevices {
    pub devices: DeviceState,
    pub display: RGB565Display,
    pub text_display: Text40Display,
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

/// Complete persistent device state. The BF tape/dispatch state belongs to the
/// outer checkpoint, and native window/terminal/file handles are recreated.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BfDevicesSnapshot {
    devices: DeviceState,
    display: RGB565Checkpoint,
    text_display: Text40Checkpoint,
    presented_frames: u64,
    host_input_enabled: bool,
    bank_size: u16,
    rng_state: u32,
    display_mode: u16,
    display_resolution: u16,
    display_enabled: bool,
    input: VecDeque<u8>,
    output: Vec<u8>,
    storage: Option<StorageCheckpoint>,
}

impl BfDevicesSnapshot {
    pub fn bank_size(&self) -> u16 {
        self.bank_size
    }
}

impl BfDevices {
    pub fn checkpoint(&mut self) -> Result<BfDevicesSnapshot, String> {
        Ok(BfDevicesSnapshot {
            devices: self.devices.checkpoint(),
            display: self.display.checkpoint()?,
            text_display: self.text_display.checkpoint(),
            presented_frames: self.presented_frames,
            host_input_enabled: self.host_input_enabled,
            bank_size: self.bank_size,
            rng_state: self.rng_state,
            display_mode: self.display_mode,
            display_resolution: self.display_resolution,
            display_enabled: self.display_enabled,
            input: self.input.clone(),
            output: self.output.clone(),
            storage: self
                .storage
                .as_mut()
                .map(Storage::checkpoint)
                .transpose()
                .map_err(|error| format!("Checkpoint storage: {error}"))?,
        })
    }

    pub fn restore_checkpoint(
        snapshot: BfDevicesSnapshot,
        storage: Option<Storage>,
    ) -> Result<Self, String> {
        if snapshot.bank_size < 32
            || !matches!(
                snapshot.display_mode,
                DISP_OFF | DISP_TTY | DISP_TEXT40 | DISP_RGB565 | DISP_RGB565_EXT
            )
            || snapshot.display.legacy_layout != (snapshot.display_mode == DISP_RGB565)
            || snapshot.display_mode == DISP_RGB565 && !snapshot.display.initialized
            || !matches!(snapshot.display_mode, DISP_RGB565 | DISP_RGB565_EXT)
                && snapshot.display.initialized
        {
            return Err("invalid BF device checkpoint configuration".into());
        }
        let mut devices = DeviceState::restore_checkpoint(snapshot.devices)?;
        devices.audio.disable();
        let display =
            RGB565Display::restore_checkpoint(snapshot.display, usize::from(snapshot.bank_size))?;
        let text_display = Text40Display::restore_checkpoint(snapshot.text_display)?;
        // Validate all other state before restoring dirty disk flags. A rejected
        // checkpoint must never cause a supplied disk to acquire pending writes.
        let storage = match (snapshot.storage, storage) {
            (None, None) => None,
            (Some(saved), Some(storage)) => Some(
                storage
                    .restore_checkpoint(saved)
                    .map_err(|error| format!("Restore storage: {error}"))?,
            ),
            _ => return Err("checkpoint disk presence differs from supplied disk".into()),
        };
        Ok(Self {
            devices,
            display,
            text_display,
            storage,
            presented_frames: snapshot.presented_frames,
            host_input_enabled: snapshot.host_input_enabled,
            bank_size: snapshot.bank_size,
            rng_state: snapshot.rng_state,
            display_mode: snapshot.display_mode,
            display_resolution: snapshot.display_resolution,
            display_enabled: snapshot.display_enabled,
            input: snapshot.input,
            output: snapshot.output,
        })
    }

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
            devices: {
                let mut devices = DeviceState::new(tick_hz)?;
                devices.audio.disable();
                devices
            },
            display: RGB565Display::new(),
            text_display: Text40Display::default(),
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
    /// Selected disk block and word address, for optional host progress reports.
    /// This observes the device without issuing a read or loading a block.
    pub fn storage_position(&self) -> Option<(u16, u16)> {
        self.storage
            .as_ref()
            .map(|storage| (storage.get_block(), storage.get_addr()))
    }
    pub fn push_input(&mut self, bytes: &[u8]) {
        self.input.extend(bytes);
    }
    pub fn take_output(&mut self) -> Vec<u8> {
        std::mem::take(&mut self.output)
    }
    pub fn window_closed(&self) -> bool {
        self.display.is_window_closed() || self.text_display.should_quit()
    }
    pub fn poll_host_input(&mut self) -> Result<(), String> {
        if self.host_input_enabled && self.display_mode == DISP_TEXT40 {
            self.text_display.poll_input()?;
            self.input.extend(self.text_display.take_input());
        }
        Ok(())
    }
    pub fn flush_storage(&mut self) -> Result<(), String> {
        if let Some(storage) = &mut self.storage {
            storage
                .flush_preserving_dirty()
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
            10..=15 => 0, // Reserved.
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
            // The Brainfuck CPU owns the interrupt controller, including its
            // MMIO shadow. Device callbacks must not alter trap state.
            HDR_IRQ_STATUS..=HDR_IRQ_CAUSE => memory.read(address),
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
                } else if value != 0 && self.display_mode == DISP_TEXT40 && self.display_enabled {
                    let sequence = self
                        .presented_frames
                        .checked_add(1)
                        .ok_or("Frame counter overflow")?;
                    self.text_display.present(memory, sequence)?;
                    self.presented_frames = sequence;
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
            HDR_IRQ_STATUS..=HDR_IRQ_CAUSE => {} // CPU microcode commits this store.
            27..=31 => {
                if self.devices.is_input_request(address, value) {
                    self.poll_device_input()?;
                }
                if let Some(frame) = self.devices.write_register_with_storage(
                    address,
                    value,
                    memory,
                    self.bank_size,
                    self.display_mode,
                    self.storage.as_mut(),
                ) {
                    self.display
                        .present(frame.width, frame.height, frame.pixels)?;
                    self.presented_frames += 1;
                }
                rvm::input::sync_display_input(&self.devices, &self.display);
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
                self.text_display.enter()?;
                self.display.disable();
            }
            _ => return Err(format!("Unsupported display mode {mode}")),
        }
        if mode != DISP_TEXT40 {
            self.text_display.leave();
        }
        self.display_mode = mode;
        Ok(())
    }

    fn poll_device_input(&mut self) -> Result<(), String> {
        if self.host_input_enabled {
            if self.display_mode == DISP_TEXT40 {
                let held = self.text_display.key_state()?;
                for key in [82, 81, 80, 79, 29, 27] {
                    let down = held == Some(key);
                    self.devices.accept_input([KeyTransition { key, down }]);
                }
            }
            rvm::input::poll_display_input(&mut self.devices, &self.display);
        }
        Ok(())
    }

    /// Complete a requested tick with explicit controller events. Window input
    /// is sampled by input commands independently of clock progression.
    pub fn complete_tick<M: GuestMemory + ?Sized>(
        &mut self,
        memory: &mut M,
        events: Vec<rvm::devices::InputTransition>,
    ) -> Result<(), String> {
        self.devices
            .complete_tick_with_memory(memory, self.bank_size, &events)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rvm::devices::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    #[test]
    fn pcm_is_not_advertised_and_audio_commands_are_explicitly_unsupported() {
        let mut host = host();
        let mut ram = vec![0; 256];
        assert!(!host.devices.audio.supported());
        command(&mut host, &mut ram, COMMAND_CAPABILITIES, 4);
        assert_eq!(ram[65] & rvm::audio::CAP_PCM, 0);
        assert_eq!(ram[65] & rvm::audio::CAP_MUSIC, 0);
        let before = ram.clone();
        store(&mut host, &mut ram, REGISTER_COMMAND, rvm::audio::CONFIGURE);
        assert_eq!(host.devices.status(), STATUS_UNSUPPORTED);
        assert_eq!(ram[63..], before[63..]);
        for command in [rvm::audio::MUSIC_BANK, rvm::audio::MUSIC_SCORE,
                        rvm::audio::MUSIC_CONTROL, rvm::audio::MUSIC_STATE] {
            store(&mut host, &mut ram, REGISTER_COMMAND, command);
            assert_eq!(host.devices.status(), STATUS_UNSUPPORTED);
            assert_eq!(ram[63..], before[63..]);
        }
        host.devices.reset();
        assert!(!host.devices.audio.supported());
        let restored = BfDevices::restore_checkpoint(host.checkpoint().unwrap(), None).unwrap();
        assert!(!restored.devices.audio.supported());
    }

    #[test]
    fn device_checkpoint_matches_continuous_rng_tty_frame_and_pending_tick() {
        let mut original = host();
        let mut memory = vec![0; 256];
        original.push_input(&[0, 255, b'A']);
        store(&mut original, &mut memory, HDR_TTY_OUT, 65);
        original.read_mmio(HDR_RNG, &mut memory).unwrap();
        store(&mut original, &mut memory, HDR_DISP_RESOLUTION, 0x0201);
        store(&mut original, &mut memory, HDR_DISP_MODE, DISP_RGB565);
        store(&mut original, &mut memory, 34, 0xf800);
        store(&mut original, &mut memory, HDR_DISP_FLUSH, 1);
        store(&mut original, &mut memory, 35, 0x001f);
        original
            .devices
            .set_script(
                InputScript::from_json(
                    r#"[{"tick":1,"key":"A","down":true},{"tick":2,"key":"A","down":false}]"#,
                )
                .unwrap(),
            )
            .unwrap();
        original
            .display
            .get_state()
            .lock()
            .unwrap()
            .set_key(82, true);
        command(&mut original, &mut memory, COMMAND_NEXT_TICK, 8);
        let bytes = serde_json::to_vec(&original.checkpoint().unwrap()).unwrap();
        let mut restored =
            BfDevices::restore_checkpoint(serde_json::from_slice(&bytes).unwrap(), None).unwrap();
        let mut copy = memory.clone();
        assert_eq!(original.take_output(), restored.take_output());
        for address in [
            HDR_TTY_IN_STATUS,
            HDR_TTY_IN_POP,
            HDR_TTY_IN_POP,
            HDR_TTY_IN_POP,
            HDR_TTY_IN_STATUS,
            HDR_RNG,
            HDR_RNG,
        ] {
            assert_eq!(
                original.read_mmio(address, &mut memory).unwrap(),
                restored.read_mmio(address, &mut copy).unwrap()
            );
        }
        original.complete_tick(&mut memory, vec![]).unwrap();
        restored.complete_tick(&mut copy, vec![]).unwrap();
        for (host, ram) in [(&mut original, &mut memory), (&mut restored, &mut copy)] {
            store(host, ram, HDR_DISP_FLUSH, 1);
            command(host, ram, COMMAND_INPUT_READ, 10);
            command(host, ram, COMMAND_NEXT_TICK, 8);
            host.complete_tick(ram, vec![]).unwrap();
        }
        assert_eq!(memory, copy);
        assert_eq!(original.display.snapshot(), restored.display.snapshot());
        assert_eq!(
            serde_json::to_value(original.checkpoint().unwrap()).unwrap(),
            serde_json::to_value(restored.checkpoint().unwrap()).unwrap()
        );
    }

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
        if command == COMMAND_INPUT_READ {
            memory[65..73].copy_from_slice(&[1, 2, 32, 0, 0, 0, 0, 0]);
        }
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
            memory[address] = 0xa55a;
            assert!(host.write_mmio(address, 1, &mut memory).is_ok());
            assert_eq!(host.read_mmio(address, &mut memory).unwrap(), 0xa55a);
        }
        assert!(host.write_mmio(HDR_DISP_MODE, 5, &mut memory).is_err());
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
        assert_eq!(host.read_mmio(10, &mut memory).unwrap(), 0);
        host.host_input_enabled = false;
        assert_eq!(host.read_mmio(10, &mut memory).unwrap(), 0);
        store(&mut host, &mut memory, HDR_DISP_MODE, DISP_RGB565_EXT);
        assert!(host.display.snapshot().is_none());
        assert!(!host.window_closed());
    }

    #[test]
    fn extended_snapshot_and_independent_input_preserve_scheduled_ticks() {
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
        command(&mut host, &mut memory, COMMAND_INPUT_READ, 10);
        assert_eq!(memory[68], 1);
        assert_eq!(&memory[160..163], &[1, 1, 82]);
        assert_eq!(host.read_mmio(10, &mut memory).unwrap(), 0);
        assert_eq!(host.devices.tick(), 0);
        assert!(!host.devices.held(40)); // Future replay remains unavailable.
        command(&mut host, &mut memory, COMMAND_NEXT_TICK, 8);
        assert_eq!(
            host.read_mmio(REGISTER_STATUS, &mut memory).unwrap(),
            STATUS_PENDING
        );
        host.complete_tick(
            &mut memory,
            vec![rvm::devices::InputTransition::Key { key: 4, down: true }],
        )
        .unwrap();
        assert_eq!(host.devices.tick(), 1);
        assert_eq!(host.read_mmio(10, &mut memory).unwrap(), 0);
        for key in [40, 4] {
            command(&mut host, &mut memory, COMMAND_INPUT_READ, 10);
            assert_eq!(memory[68], 1);
            assert_eq!(&memory[160..163], &[1, 1, key]);
            assert_eq!(memory[166], 1);
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
            assert!(state.input.overflow);
        }
        host.host_input_enabled = false;
        command(&mut host, &mut memory, COMMAND_NEXT_TICK, 8);
        host.complete_tick(&mut memory, vec![]).unwrap();
        assert!(!host.devices.held(4));
        assert!(!host.devices.overflow_seen());
        host.host_input_enabled = true;
        command(&mut host, &mut memory, COMMAND_INPUT_STATE, 28);
        assert!(host.devices.held(4));
        assert!(host.devices.overflow_seen());
        assert_eq!(host.devices.tick(), 1);
        assert_eq!(host.devices.status(), STATUS_OK);
        assert!(!host.display.get_state().lock().unwrap().input.overflow);
        assert_eq!(memory[65], 1);
        assert_eq!(memory[75] & (1 << 4), 1 << 4);
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
