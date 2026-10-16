//! CPU trap state is separate from the ordinary call ABI and device mailbox.
use super::{VMState, VM};
use crate::constants::*;
use ripple_asm::Register;

#[derive(Clone, Copy)]
pub(super) struct TrapFrame {
    pub pc: u16,
    pub pcb: u16,
    pub enable: u16,
    pub sb: u16,
    pub sp: u16,
    pub fp: u16,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Instr;

    fn i(op: u8, a: u16, b: u16, c: u16) -> Instr {
        Instr::new(op, op, a, b, c)
    }
    fn vm() -> VM {
        let mut vm = VM::with_memory_size(64, 4096);
        vm.instructions = vec![i(0x42, 0, 0, 0); 128];
        vm.state = VMState::Running;
        vm.host_input_enabled = false;
        vm.irq_vector_bank = 1;
        vm.irq_vector_off = 0;
        vm.irq_enable = 5;
        vm
    }

    #[test]
    fn entry_and_return_preserve_live_call_registers_and_switch_stacks() {
        let mut vm = vm();
        for r in 3..32 {
            vm.registers[r] = (100 + r) as u16;
        }
        vm.registers[Register::Sb as usize] = 2;
        vm.registers[Register::Sp as usize] = 63;
        vm.write_csr(CSR_IRQ_SB, 3).unwrap();
        vm.write_csr(CSR_IRQ_SP, 1).unwrap();
        let before = vm.registers;
        vm.irq_status = IRQ_SW;
        vm.step().unwrap();
        assert_eq!(vm.registers[Register::Ra as usize], before[3]);
        assert_eq!(vm.registers[Register::Rab as usize], before[4]);
        assert_eq!(vm.registers[Register::Sb as usize], 3);
        assert_eq!(vm.registers[Register::Sp as usize], 1);
        assert_eq!(vm.read_csr(CSR_SAVED_SP).unwrap(), 63);
        vm.irq_enable = 0;
        vm.handle_mmio_write(HDR_IRQ_BUSY, 1);
        vm.instructions[65] = i(0x22, 0, 0, 0);
        vm.step().unwrap();
        assert_eq!(vm.registers, before);
        assert_eq!(vm.irq_enable, 5);
        assert_eq!(vm.irq_busy, 0);
    }

    #[test]
    fn ack_cannot_interrupt_restoration_or_lose_a_reasserted_source() {
        let mut vm = vm();
        vm.irq_status = IRQ_SW;
        vm.step().unwrap();
        vm.handle_mmio_write(HDR_IRQ_STATUS, IRQ_SW | 4);
        vm.handle_mmio_write(HDR_IRQ_BUSY, 1);
        vm.step().unwrap();
        assert_eq!(vm.registers[Register::Pc as usize], 2);
        assert_eq!(vm.registers[Register::Pcb as usize], 1);
        assert_eq!(vm.irq_status, IRQ_SW | 4);
        vm.interrupt_return().unwrap();
        vm.step().unwrap();
        assert_eq!(vm.irq_cause, 1);
        assert_eq!(vm.irq_status, 4);
    }

    #[test]
    fn unacknowledged_source_is_retried_and_saved_context_is_editable() {
        let mut vm = vm();
        vm.irq_status = IRQ_SW;
        vm.step().unwrap();
        vm.write_csr(CSR_EPC, 9).unwrap();
        vm.write_csr(CSR_EPCB, 0).unwrap();
        vm.write_csr(CSR_SAVED_ENABLE, 0).unwrap();
        vm.interrupt_return().unwrap();
        assert_eq!(vm.registers[Register::Pc as usize], 9);
        assert_eq!(vm.irq_status, IRQ_SW);
        assert_eq!(vm.irq_enable, 0);
        assert!(vm.interrupt_return().is_err());
        assert!(vm.write_csr(CSR_EPC, 0).is_err());
        assert!(vm.write_csr(CSR_IRQ_SB, 0).is_err());
        assert!(vm.write_csr(CSR_IRQ_SP, 64).is_err());
    }

    #[test]
    fn stopped_batches_do_not_advance_pending_timer_state() {
        let mut vm = vm();
        vm.irq_timer_period = 97;
        vm.irq_timer_deadline = Some(97);
        vm.executed_instructions = 97;
        for state in [VMState::Halted, VMState::Breakpoint] {
            vm.state = state;
            assert_eq!(vm.run_batch(1024).unwrap(), 0);
            assert_eq!(vm.irq_status, 0);
            assert_eq!(vm.irq_timer_deadline, Some(97));
        }
    }

    #[test]
    fn timer_matches_single_step_batched_and_hot_jit_execution() {
        let make = |jit| {
            let mut vm = vm();
            vm.jit_enabled = jit;
            vm.irq_enable = IRQ_TIMER;
            vm.irq_timer_period = 97;
            vm.irq_timer_deadline = Some(97);
            vm.registers[15] = 20000;
            vm.registers[16] = 23;
            vm.registers[17] = 1;
            vm.instructions[0] = i(0x0a, 15, 15, u16::MAX);
            vm.instructions[1] = i(0x16, 15, 0, u16::MAX);
            vm.instructions[2] = i(0, 0, 0, 0);
            vm.instructions[64] = i(0x0a, 5, 5, 1);
            vm.instructions[65] = i(0x12, 17, 0, 16);
            vm.instructions[66] = i(0x22, 0, 0, 0);
            vm
        };
        let mut reference = make(false);
        let mut batch = make(false);
        let mut jit = make(true);
        for _ in 0..3000 {
            let count = batch.run_batch(1024).unwrap();
            assert_eq!(jit.run_batch(1024).unwrap(), count);
            for _ in 0..count {
                reference.step().unwrap();
            }
            for other in [&reference, &jit] {
                assert_eq!(batch.registers, other.registers);
                assert_eq!(batch.memory, other.memory);
                assert_eq!(batch.executed_instructions, other.executed_instructions);
                assert_eq!(batch.irq_status, other.irq_status);
                assert_eq!(batch.irq_busy, other.irq_busy);
            }
            if matches!(batch.state, VMState::Halted) {
                break;
            }
        }
        assert!(matches!(batch.state, VMState::Halted));
        assert!(batch.registers[5] > 200);
        assert!(jit.jit_stats().unwrap().native_instructions > 0);
    }
}

impl VM {
    pub(super) fn update_irq_timer(&mut self) {
        if let Some(deadline) = self.irq_timer_deadline {
            if self.executed_instructions >= deadline {
                self.irq_status |= IRQ_TIMER;
                let period = u64::from(self.irq_timer_period);
                let elapsed = self.executed_instructions - deadline;
                self.irq_timer_deadline = self
                    .executed_instructions
                    .checked_add(period - elapsed % period);
                self.sync_irq_memory();
            }
        }
    }

    pub(super) fn interrupt_budget(&self, budget: u64) -> u64 {
        self.irq_timer_deadline.map_or(budget, |deadline| {
            budget.min(deadline.saturating_sub(self.executed_instructions).max(1))
        })
    }

    pub(super) fn read_csr(&self, csr: u16) -> Result<u16, String> {
        let frame = self.irq_frame;
        Ok(match csr {
            CSR_EPC => frame.map_or(0, |f| f.pc),
            CSR_EPCB => frame.map_or(0, |f| f.pcb),
            CSR_SAVED_ENABLE => frame.map_or(0, |f| f.enable),
            CSR_TIMER_LO => self.irq_timer_period as u16,
            CSR_TIMER_HI => (self.irq_timer_period >> 16) as u16,
            CSR_IRQ_SB => self.irq_stack_bank,
            CSR_IRQ_SP => self.irq_stack_top,
            CSR_SAVED_SB => frame.map_or(0, |f| f.sb),
            CSR_SAVED_SP => frame.map_or(0, |f| f.sp),
            CSR_SAVED_FP => frame.map_or(0, |f| f.fp),
            CSR_PENDING => self.irq_status,
            CSR_IRQ_VERSION => 2,
            _ => return Err(format!("unknown CPU control register {csr}")),
        })
    }

    pub(super) fn write_csr(&mut self, csr: u16, value: u16) -> Result<(), String> {
        match csr {
            CSR_EPC | CSR_EPCB | CSR_SAVED_ENABLE | CSR_SAVED_SB | CSR_SAVED_SP | CSR_SAVED_FP => {
                let frame = self
                    .irq_frame
                    .as_mut()
                    .ok_or("trap state write outside an interrupt")?;
                match csr {
                    CSR_EPC => frame.pc = value,
                    CSR_EPCB => frame.pcb = value,
                    CSR_SAVED_ENABLE => frame.enable = value,
                    CSR_SAVED_SB => frame.sb = value,
                    CSR_SAVED_SP => frame.sp = value,
                    CSR_SAVED_FP => frame.fp = value,
                    _ => unreachable!(),
                }
            }
            CSR_TIMER_LO | CSR_TIMER_HI => {
                self.irq_timer_period = if csr == CSR_TIMER_LO {
                    (self.irq_timer_period & 0xffff0000) | u32::from(value)
                } else {
                    (self.irq_timer_period & 0xffff) | (u32::from(value) << 16)
                };
                // The configuration instruction itself is not a timer tick.
                self.irq_timer_deadline = (self.irq_timer_period != 0).then(|| {
                    self.executed_instructions
                        .saturating_add(1 + u64::from(self.irq_timer_period))
                });
            }
            CSR_IRQ_SB => {
                if value == 0
                    || usize::from(value) * usize::from(self.bank_size) >= self.memory.len()
                {
                    return Err("interrupt stack bank is not usable RAM".into());
                }
                self.irq_stack_bank = value;
            }
            CSR_IRQ_SP => {
                if value != 0
                    && (value >= self.bank_size
                        || self.irq_stack_bank == 0
                        || usize::from(self.irq_stack_bank) * usize::from(self.bank_size)
                            + usize::from(value)
                            >= self.memory.len())
                {
                    return Err("interrupt stack top is outside its RAM bank".into());
                }
                self.irq_stack_top = value;
            }
            CSR_PENDING => {
                self.irq_status &= !value;
                self.sync_irq_memory();
            }
            CSR_IRQ_VERSION => return Err("IRQ version is read-only".into()),
            _ => return Err(format!("unknown CPU control register {csr}")),
        }
        Ok(())
    }

    pub(super) fn interrupt_return(&mut self) -> Result<(), String> {
        let frame = self.irq_frame.take().ok_or("IRET outside an interrupt")?;
        if !self.irq_acknowledged {
            self.irq_status |= 1 << (self.irq_cause - 1);
        }
        self.registers[Register::Pc as usize] = frame.pc;
        self.registers[Register::Pcb as usize] = frame.pcb;
        self.registers[Register::Sb as usize] = frame.sb;
        self.registers[Register::Sp as usize] = frame.sp;
        self.registers[Register::Fp as usize] = frame.fp;
        self.irq_enable = frame.enable;
        self.irq_busy = 0;
        self.irq_cause = 0;
        self.irq_acknowledged = false;
        self.skip_pc_increment = true;
        self.sync_irq_memory();
        Ok(())
    }

    pub(super) fn trap_error(&mut self, error: String) -> String {
        self.state = VMState::Error(error.clone());
        error
    }
}
