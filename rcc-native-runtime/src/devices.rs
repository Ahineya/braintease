//! Native memory adapter over the existing synchronous device/display services.
use super::*;
use rvm::constants::*;
impl Runtime {
    pub(super) fn read(&mut self, index: usize) -> u16 {
        if index < 32 {
            return match index {
                HDR_TTY_OUT | HDR_DISP_FLUSH => 0,
                HDR_TTY_STATUS => 1,
                HDR_TTY_IN_POP | HDR_TTY_IN_STATUS => 0,
                HDR_RNG => {
                    self.rng = self.rng.wrapping_mul(1664525).wrapping_add(1013904223);
                    (self.rng >> 16) as u16
                }
                HDR_RNG_SEED => self.rng as u16,
                HDR_DISP_MODE => self.mode,
                HDR_DISP_STATUS => 1 | if self.flushed { 2 } else { 0 },
                HDR_DISP_CTL => u16::from(self.enabled),
                HDR_KEY_UP..=HDR_KEY_X => {
                    if self.mode == DISP_RGB565_EXT {
                        self.poll_device_input();
                    }
                    let key = [82, 81, 80, 79, 29, 27][index - HDR_KEY_UP];
                    if self.mode == DISP_RGB565 && self.host.visual() {
                        u16::from(self.display.get_state().lock().unwrap().keys_held[key])
                    } else {
                        u16::from(self.devices.held(key as u16))
                    }
                }
                HDR_DISP_RESOLUTION => self.resolution,
                HDR_STORE_BLOCK..=HDR_STORE_CTL => fatal("native v1 storage device is unavailable"),
                HDR_IRQ_STATUS..=HDR_IRQ_CAUSE => 0,
                27..=31 => self.devices.read_register(index).unwrap_or(0),
                _ => unreachable!(),
            };
        }
        if self.mode == DISP_RGB565 && index < BANK {
            if let Some(value) = self.display.read_memory(index) {
                return value;
            }
        }
        self.memory[index]
    }
    pub(super) fn write(&mut self, index: usize, value: u16) {
        if index < 32 {
            match index {
                HDR_TTY_OUT => {
                    self.putc(u64::from(value));
                }
                HDR_RNG_SEED => self.rng = (self.rng & 0xffff0000) | u32::from(value),
                HDR_DISP_RESOLUTION => self.resolution = value,
                HDR_DISP_MODE => {
                    if value == DISP_TEXT40 {
                        fatal("native v1 TEXT40 rendering is unavailable; use RGB565");
                    }
                    if value > DISP_RGB565_EXT {
                        fatal("unsupported native display mode");
                    }
                    if value != self.mode {
                        if value == DISP_RGB565 {
                            self.display
                                .init((self.resolution >> 8) as u8, self.resolution as u8, BANK)
                                .unwrap_or_else(|e| fatal(&e));
                        } else if value != DISP_RGB565_EXT {
                            self.display.disable();
                        }
                        self.mode = value;
                    }
                    self.request_display_if_enabled();
                }
                HDR_DISP_CTL => {
                    if value & 1 != 0 {
                        self.enabled = true;
                    }
                    if value & 2 != 0 {
                        self.memory[32..1032].fill(0);
                    }
                    self.request_display_if_enabled();
                }
                HDR_DISP_FLUSH => {
                    if value != 0 {
                        self.flushed = false;
                        if self.mode == DISP_RGB565 && self.display.flush() {
                            self.capture();
                        }
                        self.flushed = true;
                    }
                }
                HDR_STORE_BLOCK..=HDR_STORE_CTL => fatal("native v1 storage device is unavailable"),
                HDR_IRQ_STATUS..=HDR_IRQ_VECTOR_OFF if value != 0 => {
                    fatal("native v1 interrupts are unavailable")
                }
                27..=31 => {
                    if self.devices.is_input_request(index, value) {
                        self.poll_device_input();
                    }
                    if let Some(frame) = self.devices.write_register(
                        index,
                        value,
                        &mut self.memory,
                        BANK as u16,
                        self.mode,
                    ) {
                        self.display
                            .present(frame.width, frame.height, frame.pixels)
                            .unwrap_or_else(|e| fatal(&e));
                        self.capture();
                    }
                    if self.devices.has_pending_tick() {
                        if self.host.paced() {
                            let origin =
                                *self.clock_start.get_or_insert_with(std::time::Instant::now);
                            let target = origin
                                + std::time::Duration::from_secs_f64(
                                    (self.devices.tick() + 1) as f64
                                        / f64::from(self.devices.tick_hz()),
                                );
                            if let Some(delay) =
                                target.checked_duration_since(std::time::Instant::now())
                            {
                                std::thread::sleep(delay);
                            }
                        }
                        self.devices
                            .complete_tick(&mut self.memory, BANK as u16, &[])
                            .unwrap_or_else(|e| fatal(&e));
                    }
                }
                _ => {} // Defined read-only device registers ignore writes.
            }
            return;
        }
        if self.mode == DISP_RGB565 && index < BANK && self.display.write_memory(index, value) {
            return;
        }
        self.memory[index] = value;
    }
    fn poll_device_input(&mut self) {
        rvm::input::poll_display_input(&mut self.devices, &self.display);
    }
    fn request_display_if_enabled(&mut self) {
        if self.enabled && matches!(self.mode, DISP_RGB565 | DISP_RGB565_EXT) {
            self.host.request_window();
        }
    }
    fn capture(&mut self) {
        // Publishing also implies graphics use for callers of the legacy flush
        // or PRESENT interface that do not explicitly set the enable bit.
        self.host.request_window();
        self.presentations += 1;
        if self.presentations != self.frame_target {
            return;
        }
        if let Some(path) = std::env::var_os("RCC_NATIVE_FRAME") {
            let frame = self
                .display
                .snapshot()
                .unwrap_or_else(|| fatal("native display has no completed frame"));
            let bytes: Vec<u8> = frame.pixels.iter().flat_map(|p| p.to_le_bytes()).collect();
            std::fs::write(path, bytes)
                .unwrap_or_else(|e| fatal(&format!("native frame capture: {e}")));
            self.flush();
            std::process::exit(0);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn native_input_commands_poll_window_without_a_tick_grant() {
        let mut rt = Runtime::new();
        let desc = BANK + 10;
        rt.memory[desc..desc + 8].copy_from_slice(&[1, 8, 0, 0, 0, 0, 0, 0]);
        rt.write(27, 1);
        rt.write(28, 10);
        rt.display.get_state().lock().unwrap().set_key(82, true);
        rt.write(29, rvm::devices::COMMAND_INPUT_POP);
        assert_eq!(&rt.memory[desc + 2..desc + 8], &[1, 82, 1, 0, 0, 0]);
        assert_eq!(rt.devices.tick(), 0);
        rt.write(29, rvm::devices::COMMAND_INPUT_POP);
        assert_eq!(rt.memory[desc + 2], 0);
        rt.display.get_state().lock().unwrap().set_key(82, false);
        rt.memory[desc + 1] = 19;
        rt.write(29, rvm::devices::COMMAND_INPUT_STATE);
        assert_eq!(rt.memory[desc + 3 + 82 / 16] & (1 << (82 % 16)), 0);
        assert_eq!(rt.devices.tick(), 0);
        rt.memory[desc + 1] = 8;
        rt.write(29, rvm::devices::COMMAND_INPUT_POP);
        assert_eq!(&rt.memory[desc + 2..desc + 8], &[1, 82, 0, 0, 0, 0]);
        rt.display.get_state().lock().unwrap().set_key(4, true);
        rt.write(29, rvm::devices::COMMAND_NEXT_TICK);
        assert_eq!(rt.devices.tick(), 1);
        assert!(!rt.devices.held(4)); // Clock grants do not collect live input.
        rt.write(29, rvm::devices::COMMAND_INPUT_POP);
        assert_eq!(&rt.memory[desc + 2..desc + 8], &[1, 4, 1, 1, 0, 0]);
    }
}
