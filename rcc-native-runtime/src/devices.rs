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
                    let key = [82, 81, 80, 79, 29, 27][index - HDR_KEY_UP];
                    if self.mode == DISP_RGB565 && self.visual {
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
                }
                HDR_DISP_CTL => {
                    if value & 1 != 0 {
                        self.enabled = true;
                    }
                    if value & 2 != 0 {
                        self.memory[32..1032].fill(0);
                    }
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
                        let events = {
                            let shared = self.display.get_state();
                            let mut state = shared.lock().unwrap();
                            std::mem::take(&mut state.pending_keys)
                                .into_iter()
                                .map(|(key, down)| rvm::devices::KeyTransition { key, down })
                                .collect::<Vec<_>>()
                        };
                        self.devices
                            .complete_tick(&mut self.memory, BANK as u16, &events)
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
    fn capture(&mut self) {
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
