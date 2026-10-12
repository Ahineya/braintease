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

    /// Grant one outstanding NEXT_TICK, latching input at the grant boundary.
    pub fn complete_tick(&mut self, mut events: Vec<KeyTransition>) -> Result<(), String> {
        if self.host_input_enabled {
            if let Some(display) = self.rgb565_display.as_ref() {
                let shared = display.get_state();
                let mut state = shared.lock().unwrap();
                if state.key_overflow {
                    self.devices.mark_input_overflow();
                    // The bitmap remains authoritative if host transitions overflowed.
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
        }
        self.devices
            .complete_tick(&mut self.memory, self.bank_size, &events)
    }

    pub fn window_closed(&self) -> bool {
        self.rgb565_display
            .as_ref()
            .is_some_and(|d| d.get_state().lock().unwrap().should_quit)
    }
}
