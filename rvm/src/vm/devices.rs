use super::VM;
use crate::constants::DISP_RGB565;
use crate::devices::KeyTransition;

impl VM {
    /// Access a guest cell with the same routing used by LOAD.
    pub fn read_cell(&mut self, bank: u16, offset: u16) -> Result<u16, String> {
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

    /// Collect live input independently of logical time. Deterministic hosts
    /// leave window events untouched and use the scheduled replay/controller.
    pub(super) fn poll_device_input(&mut self) {
        if self.host_input_enabled {
            if let Some(display) = &self.rgb565_display {
                crate::input::poll_display_input(&mut self.devices, display);
            }
        }
    }

    /// Grant a tick with explicit controller events; do not sample the window.
    pub fn complete_tick(&mut self, events: Vec<KeyTransition>) -> Result<(), String> {
        self.devices.complete_tick(&mut self.memory, self.bank_size, &events)
    }

    pub fn window_closed(&self) -> bool {
        self.rgb565_display
            .as_ref()
            .is_some_and(|d| d.is_window_closed())
    }
}
