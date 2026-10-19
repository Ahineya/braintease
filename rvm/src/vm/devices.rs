use super::VM;
use crate::constants::DISP_RGB565;
use crate::devices::KeyTransition;

impl VM {
    /// Access a guest cell with the same routing used by LOAD.
    pub fn read_cell(&mut self, bank: u16, offset: u16) -> Result<u16, String> {
        if bank == 0 && usize::from(offset) == crate::constants::HDR_MATH_STACK {
            return Ok(1);
        }
        if self.mmu.mapped(bank, crate::mmu::Access::Read) {
            let address = self.mmu_translate(bank, offset, crate::mmu::Access::Read)?;
            return self.memory.get(address).copied().ok_or("translated LOAD outside RAM".into());
        }
        if bank == 0 {
            if let Some(value) = self.handle_mmio_read(offset as usize) {
                return Ok(value);
            }
            if self.display_mode == DISP_RGB565 {
                if let Some(value) = self
                    .rgb565_display
                    .as_ref()
                    .and_then(|d| d.read_memory(offset as usize))
                {
                    return Ok(value);
                }
            }
        }
        let address = bank as usize * self.bank_size as usize + offset as usize;
        self.memory
            .get(address)
            .copied()
            .ok_or_else(|| format!("LOAD: memory address out of bounds: {address}"))
    }

    /// Access a guest cell with the same routing used by STORE.
    pub fn write_cell(&mut self, bank: u16, offset: u16, value: u16) -> Result<(), String> {
        if bank == 0 && usize::from(offset) == crate::constants::HDR_MATH_STACK {
            return self.stack_math(value);
        }
        if self.mmu.mapped(bank, crate::mmu::Access::Write) {
            let address = self.mmu_translate(bank, offset, crate::mmu::Access::Write)?;
            *self.memory.get_mut(address).ok_or("translated STORE outside RAM")? = value;
            return Ok(());
        }
        if bank == 0 {
            if self.handle_mmio_write(offset as usize, value) {
                return Ok(());
            }
            if self.display_mode == DISP_RGB565 {
                if let Some(display) = self.rgb565_display.as_mut() {
                    if display.write_memory(offset as usize, value) {
                        return Ok(());
                    }
                }
            }
        }
        let address = bank as usize * self.bank_size as usize + offset as usize;
        let cell = self
            .memory
            .get_mut(address)
            .ok_or_else(|| format!("STORE: memory address out of bounds: {address}"))?;
        *cell = value;
        Ok(())
    }

    fn stack_math(&mut self, offset: u16) -> Result<(), String> {
        use crate::{devices::*, mmu::{Access, Fault}};
        let bank = self.registers[ripple_asm::Register::Sb as usize];
        if self.bank_size == 0 { return Err("math stack requires nonzero bank size".into()); }
        let mut addresses = [0usize; crate::float::CELLS];
        let mut packet = [0u16; crate::float::CELLS];
        for i in 0..crate::float::CELLS {
            let index = usize::from(offset) + i;
            let target_bank = usize::from(bank) + index / usize::from(self.bank_size);
            let target_offset = (index % usize::from(self.bank_size)) as u16;
            if offset >= self.bank_size || target_bank > usize::from(u16::MAX) ||
                (!self.mmu.mapped(bank, Access::Read) && target_bank == 0 && target_offset < 32) {
                self.mmu_pending = Some(Fault { cause: crate::mmu::CAUSE_READ, bank, offset });
                return Err("invalid math stack packet".into());
            }
            let at = self.mmu_translate(target_bank as u16, target_offset, Access::Read)?;
            let write = self.mmu_translate(target_bank as u16, target_offset, Access::Write)?;
            if at != write || at >= self.memory.len() {
                self.mmu_pending = Some(Fault { cause: crate::mmu::CAUSE_WRITE,
                    bank: target_bank as u16, offset: target_offset });
                return Err("math stack packet outside writable RAM".into());
            }
            addresses[i] = at;
            packet[i] = self.memory[at];
        }
        let result = if packet[0] != ABI_VERSION { Err(STATUS_VERSION) }
            else if usize::from(packet[1]) != crate::float::CELLS { Err(STATUS_BAD_DESCRIPTOR) }
            else { crate::float::execute(&mut packet) };
        match result {
            Ok(()) => {
                for i in 12..16 { self.memory[addresses[i]] = packet[i]; }
                self.memory[addresses[3]] = STATUS_OK;
            }
            Err(status) => self.memory[addresses[3]] = status,
        }
        Ok(())
    }

    /// Collect live input independently of logical time. Deterministic hosts
    /// leave window events untouched and use the scheduled replay/controller.
    pub(super) fn poll_device_input(&mut self) {
        if self.host_input_enabled {
            if self.display_mode == crate::constants::DISP_TEXT40 {
                self.poll_keyboard();
                self.keyboard_state.last_read_counter += 1;
                if self.keyboard_state.last_read_counter > 10 {
                    self.clear_keyboard_state();
                }
                let k = self.keyboard_state;
                self.devices.accept_input([
                    KeyTransition {
                        key: 82,
                        down: k.up,
                    },
                    KeyTransition {
                        key: 81,
                        down: k.down,
                    },
                    KeyTransition {
                        key: 80,
                        down: k.left,
                    },
                    KeyTransition {
                        key: 79,
                        down: k.right,
                    },
                    KeyTransition { key: 29, down: k.z },
                    KeyTransition { key: 27, down: k.x },
                ]);
            }
            if let Some(display) = &self.rgb565_display {
                crate::input::poll_display_input(&mut self.devices, display);
            }
        }
    }

    /// Grant a tick with explicit controller events; do not sample the window.
    pub fn complete_tick(
        &mut self,
        events: Vec<crate::devices::InputTransition>,
    ) -> Result<(), String> {
        self.devices
            .complete_tick(&mut self.memory, self.bank_size, &events)
    }

    pub fn window_closed(&self) -> bool {
        self.rgb565_display
            .as_ref()
            .is_some_and(|d| d.is_window_closed())
    }
}

/// An explicit view of both architectural memories. Code edits are returned to
/// the VM so the JIT can invalidate the entire range once after submission.
pub(super) struct DeviceMemory<'a> {
    pub ram: &'a mut Vec<u16>,
    pub code: &'a mut Vec<super::Instr>,
    pub changed: Option<std::ops::Range<usize>>,
    pub code_limit: usize,
    pub output: &'a mut std::collections::VecDeque<u8>,
}
impl crate::devices::GuestMemory for DeviceMemory<'_> {
    fn len(&self) -> usize {
        self.ram.len()
    }
    fn read(&self, i: usize) -> u16 {
        self.ram[i]
    }
    fn write(&mut self, i: usize, v: u16) {
        self.ram[i] = v;
    }
    #[cfg(target_arch = "wasm32")]
    fn buffer_console_output(&mut self, _bytes: &[u8]) -> bool { true }
    fn record_console_output(&mut self, bytes: &[u8]) {
        self.output.extend(bytes);
    }
    fn code_capacity(&self, size: u16) -> usize {
        self.code_limit.min(usize::from(size) * 65536)
    }
    fn write_code(&mut self, size: u16, start: usize, code: &[[u16; 4]]) -> Result<(), u16> {
        use crate::devices::STATUS_BOUNDS;
        let end = start
            .checked_add(code.len())
            .filter(|&end| end <= self.code_capacity(size))
            .ok_or(STATUS_BOUNDS)?;
        if code.is_empty() {
            return Ok(());
        }
        if end > self.code.len() {
            let length = end
                .div_ceil(usize::from(size))
                .saturating_mul(usize::from(size))
                .min(self.code_capacity(size));
            self.code
                .try_reserve(length - self.code.len())
                .map_err(|_| STATUS_BOUNDS)?;
            self.code.resize(
                length,
                super::Instr {
                    opcode: 0x42,
                    word0: 0x42,
                    word1: 0,
                    word2: 0,
                    word3: 0,
                },
            );
        }
        for (i, w) in (start..end).zip(code) {
            self.code[i] = super::Instr {
                opcode: (w[0] & 255) as u8,
                word0: (w[0] & 255) as u8,
                word1: w[1],
                word2: w[2],
                word3: w[3],
            };
        }
        self.changed = Some(start..end);
        Ok(())
    }
}
