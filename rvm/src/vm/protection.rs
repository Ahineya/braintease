use super::{interrupts::TrapFrame, VMState, VM};
use crate::mmu::{Access, Fault};
use ripple_asm::Register;
impl VM {
    pub(super) fn mmu_translate(
        &mut self,
        bank: u16,
        offset: u16,
        access: Access,
    ) -> Result<usize, String> {
        let limit = if matches!(access, Access::Execute | Access::CodeRead) {
            self.instructions.len()
        } else {
            self.memory.len()
        };
        self.mmu
            .translate(&self.memory, self.bank_size, bank, offset, access, limit)
            .map_err(|fault| {
                self.mmu_pending = Some(fault);
                "CPU memory fault".to_string()
            })
    }
    pub(super) fn exception(&mut self, fault: Fault, next: bool) -> Result<(), String> {
        if !self.mmu.enabled || self.irq_busy != 0 || self.irq_stack_top == 0 {
            return Err(self.trap_error(format!("unhandled/double CPU fault: {fault:?}")));
        }
        let mut pc = self.registers[Register::Pc as usize];
        let mut pcb = self.registers[Register::Pcb as usize];
        if next {
            if u32::from(pc) + 1 >= u32::from(self.bank_size) {
                pc = 0;
                pcb = pcb.wrapping_add(1);
            } else {
                pc += 1;
            }
        }
        self.irq_frame = Some(TrapFrame {
            pc,
            pcb,
            enable: self.irq_enable,
            sb: self.registers[Register::Sb as usize],
            sp: self.registers[Register::Sp as usize],
            fp: self.registers[Register::Fp as usize],
            user: self.mmu.user,
            exception: true,
        });
        self.mmu.user = false;
        self.mmu.cause = fault.cause;
        self.mmu.address = (fault.bank, fault.offset);
        self.irq_busy = 1;
        self.irq_cause = 0;
        self.irq_acknowledged = true;
        self.registers[Register::Sb as usize] = self.irq_stack_bank;
        self.registers[Register::Sp as usize] = self.irq_stack_top;
        self.registers[Register::Fp as usize] = self.irq_stack_top;
        self.registers[Register::Pcb as usize] = self.mmu.vector.0;
        self.registers[Register::Pc as usize] = self.mmu.vector.1;
        self.skip_pc_increment = true;
        self.state = VMState::Running;
        self.sync_irq_memory();
        Ok(())
    }
    pub(super) fn privilege_fault(&mut self) -> Result<(), String> {
        self.exception(
            Fault {
                cause: crate::mmu::CAUSE_PRIVILEGE,
                bank: self.registers[2],
                offset: self.registers[1],
            },
            false,
        )
    }
    pub(super) fn instruction_index(&mut self) -> Result<usize, String> {
        self.mmu_translate(self.registers[2], self.registers[1], Access::Execute)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Instr;
    use crate::{constants::*, mmu::*};
    fn i(op: u8, a: u16, b: u16, c: u16) -> Instr {
        Instr::new(op, op, a, b, c)
    }
    fn machine(jit: bool) -> VM {
        let mut vm = VM::with_memory_size(1024, 16 * 1024);
        vm.host_input_enabled = false;
        vm.state = VMState::Running;
        vm.jit_enabled = jit;
        vm.instructions = vec![i(0x42, 0, 0, 0); 4 * 1024];
        vm.memory[1024..1026].copy_from_slice(&[2, 0]);
        vm.memory[2048 + 8 * 4..2048 + 8 * 4 + 4].copy_from_slice(&[
            6,
            0,
            PRESENT | READ | WRITE | USER,
            0,
        ]);
        vm.memory[3072..3074].copy_from_slice(&[4, 0]);
        vm.memory[4096..4100].copy_from_slice(&[1, 0, PRESENT | READ | EXECUTE | USER, 0]);
        vm.write_csr(CSR_DROOT_BANK, 1).unwrap();
        vm.write_csr(CSR_IROOT_BANK, 3).unwrap();
        vm.write_csr(CSR_IRQ_SB, 5).unwrap();
        vm.write_csr(CSR_IRQ_SP, 1).unwrap();
        vm.write_csr(CSR_MMU_CONTROL, 1).unwrap();
        vm.mmu.user = true;
        vm.registers[Register::Sb as usize] = 8;
        vm.registers[Register::Sp as usize] = 27;
        vm
    }
    #[test]
    fn translation_and_user_permissions_cannot_reach_devices_or_tables() {
        let mut vm = machine(false);
        vm.memory[6 * 1024 + 7] = 123;
        assert_eq!(vm.read_cell(8, 7).unwrap(), 123);
        vm.write_cell(8, 7, 456).unwrap();
        assert_eq!(vm.memory[6 * 1024 + 7], 456);
        assert!(vm.read_cell(0, 0).is_err());
        assert!(vm.write_cell(1, 0, 1).is_err());
        vm.memory[2048 + 8 * 4] = 0;
        vm.mmu.flush();
        assert!(vm.write_cell(8, 7, 1).is_err());
        assert!(vm.read_cell(8, 1024).is_err());
    }
    #[test]
    fn syscall_preserves_next_pc_and_iret_restores_user_mode_and_stack() {
        let mut vm = machine(false);
        vm.instructions[1024] = i(0x25, 0, 0, 0);
        vm.instructions[0] = i(0x22, 0, 0, 0);
        vm.step().unwrap();
        assert!(!vm.mmu.user);
        assert_eq!(vm.mmu.cause, CAUSE_SYSCALL);
        assert_eq!(vm.read_csr(CSR_EPC).unwrap(), 1);
        assert_eq!(vm.read_csr(CSR_SAVED_MODE).unwrap(), 1);
        assert_eq!(vm.registers[Register::Sb as usize], 5);
        vm.step().unwrap();
        assert!(vm.mmu.user);
        assert_eq!(vm.registers[1], 1);
        assert_eq!(vm.registers[Register::Sb as usize], 8);
        assert_eq!(vm.registers[Register::Sp as usize], 27);
    }
    #[test]
    fn faults_are_precise_retryable_and_include_the_virtual_address() {
        let mut vm = machine(false);
        vm.instructions[1024] = i(0x11, 5, 15, 16);
        vm.instructions[0] = i(0x22, 0, 0, 0);
        vm.registers[5] = 99;
        vm.registers[15] = 7;
        vm.registers[16] = 9;
        vm.step().unwrap();
        assert_eq!(vm.mmu.cause, CAUSE_READ);
        assert_eq!(vm.mmu.address, (7, 9));
        assert_eq!(vm.registers[5], 99);
        assert_eq!(vm.read_csr(CSR_EPC).unwrap(), 0);
        vm.memory[2048 + 7 * 4..2048 + 7 * 4 + 4].copy_from_slice(&[
            6,
            0,
            PRESENT | READ | USER,
            0,
        ]);
        vm.memory[6 * 1024 + 9] = 42;
        vm.mmu.flush();
        vm.step().unwrap();
        vm.step().unwrap();
        assert!(vm.mmu.user);
        assert_eq!(vm.registers[5], 42);
        assert_eq!(vm.registers[1], 1);
    }
    #[test]
    fn user_cannot_modify_cpu_controls_code_or_halt_the_machine() {
        for inst in [
            i(0x24, 15, CSR_MMU_CONTROL, 0),
            i(0x21, 0, 15, 16),
            i(0, 0, 0, 0),
            i(0x22, 0, 0, 0),
            i(0x23, 5, CSR_DROOT_BANK, 0),
        ] {
            let mut vm = machine(false);
            vm.instructions[1024] = inst;
            vm.step().unwrap();
            assert_eq!(vm.mmu.cause, CAUSE_PRIVILEGE);
            assert!(!vm.mmu.user);
            assert!(matches!(vm.state, VMState::Running));
        }
    }
    #[test]
    fn paged_mappings_cow_and_supervisor_window_obey_permissions() {
        let mut vm = machine(false);
        let flags = PRESENT | READ | WRITE | USER;
        vm.memory[2048 + 8 * 4..2048 + 8 * 4 + 4].copy_from_slice(&[7, 0, flags | PAGED, 0]);
        vm.memory[7 * 1024..7 * 1024 + 8].copy_from_slice(&[
            6,
            512,
            flags & !WRITE | COW,
            0,
            9,
            0,
            flags,
            0,
        ]);
        vm.memory[6 * 1024 + 512 + 3] = 12;
        assert_eq!(vm.read_cell(8, 3).unwrap(), 12);
        assert!(vm.write_cell(8, 3, 13).is_err());
        vm.mmu_pending = None;
        vm.mmu.user = false;
        vm.write_csr(CSR_WINDOW_LO, 8).unwrap();
        vm.write_csr(CSR_WINDOW_HI, 8).unwrap();
        vm.write_csr(CSR_MMU_CONTROL, 3).unwrap();
        assert!(vm.write_cell(8, 3, 13).is_err());
        vm.memory[7 * 1024 + 2] = flags;
        vm.write_csr(CSR_MMU_FLUSH, 1).unwrap();
        vm.write_cell(8, 3, 13).unwrap();
        vm.write_cell(8, 515, 77).unwrap();
        assert_eq!(vm.memory[6 * 1024 + 515], 13);
        assert_eq!(vm.memory[9 * 1024 + 3], 77);
        assert_eq!(vm.read_cell(6, 515).unwrap(), 13);
    }
    #[test]
    fn last_partial_page_never_spills_into_another_bank() {
        let mut memory = vec![0; 16 * 1000];
        memory[1000..1002].copy_from_slice(&[2, 0]);
        memory[2000 + 8 * 4..2000 + 8 * 4 + 4].copy_from_slice(&[
            4,
            0,
            PRESENT | READ | USER | PAGED,
            0,
        ]);
        memory[4000..4004].copy_from_slice(&[6, 512, PRESENT | READ | USER, 0]);
        let mut mmu = Mmu::default();
        mmu.enabled = true;
        mmu.user = true;
        mmu.data_root = (1, 0);
        assert_eq!(
            mmu.translate(&memory, 1000, 8, 487, Access::Read, memory.len())
                .unwrap(),
            6999
        );
        assert!(mmu
            .translate(&memory, 1000, 8, 488, Access::Read, memory.len())
            .is_err());
    }
    #[test]
    fn translated_hot_loops_and_native_faults_match_the_interpreter() {
        let make = |jit| {
            let mut vm = machine(jit);
            vm.registers[15] = 6000;
            vm.registers[16] = 8;
            vm.registers[17] = 7;
            vm.instructions[1024] = i(0x11, 5, 16, 17);
            vm.instructions[1025] = i(0x0a, 15, 15, u16::MAX);
            vm.instructions[1026] = i(0x16, 15, 0, (-2i16) as u16);
            vm.instructions[1027] = i(0x25, 0, 0, 0);
            vm
        };
        let mut a = make(false);
        let mut b = make(true);
        for _ in 0..100 {
            assert_eq!(a.run_batch(511).unwrap(), b.run_batch(511).unwrap());
            assert_eq!(a.registers, b.registers);
            if a.mmu.cause == CAUSE_SYSCALL {
                break;
            }
        }
        assert_eq!(a.mmu.cause, CAUSE_SYSCALL);
        assert!(b.jit_stats().unwrap().native_instructions > 0);
        // Resume the already compiled loop and remove READ permission. A hot
        // native helper must publish the faulting PC without changing RV0.
        for vm in [&mut a, &mut b] {
            vm.memory[2048 + 8 * 4 + 2] = PRESENT | USER;
            vm.write_csr(CSR_MMU_FLUSH, 1).unwrap();
            vm.write_csr(CSR_EPC, 0).unwrap();
            vm.interrupt_return().unwrap();
            vm.registers[5] = 123;
            vm.registers[15] = 10;
            vm.run_batch(100).unwrap();
        }
        assert_eq!(a.registers, b.registers);
        assert_eq!(a.mmu.cause, CAUSE_READ);
        assert_eq!(b.mmu.cause, CAUSE_READ);
        assert_eq!(b.registers[5], 123);
        assert_eq!(a.executed_instructions, b.executed_instructions);
    }
    #[test]
    fn hot_user_store_cannot_bypass_cow_after_a_tlb_flush() {
        let make=|jit| {
            let mut vm=machine(jit);
            vm.registers[15]=6000;vm.registers[16]=8;vm.registers[17]=7;
            vm.instructions[1024..1030].copy_from_slice(&[
                i(0x11,5,16,17),i(0x0a,5,5,1),i(0x12,5,16,17),
                i(0x0a,15,15,u16::MAX),i(0x16,15,0,(-4i16) as u16),i(0x25,0,0,0)]);
            vm
        };
        let mut reference=make(false);let mut native=make(true);
        for vm in [&mut reference,&mut native] {
            while vm.mmu.user {vm.run_batch(511).unwrap();}
            assert_eq!(vm.memory[6*1024+7],6000);
            vm.memory[2048+8*4+2]=PRESENT|READ|USER|COW;
            vm.write_csr(CSR_MMU_FLUSH,1).unwrap();
            vm.write_csr(CSR_EPC,0).unwrap();vm.interrupt_return().unwrap();
            vm.registers[15]=10;
            while vm.mmu.user {vm.run_batch(511).unwrap();}
            assert_eq!(vm.mmu.cause,CAUSE_WRITE);
            assert_eq!(vm.read_csr(CSR_EPC).unwrap(),2);
            assert_eq!(vm.memory[6*1024+7],6000);
        }
        assert_eq!(reference.registers,native.registers);
        assert_eq!(reference.executed_instructions,native.executed_instructions);
        assert!(native.jit_stats().unwrap().native_instructions>1000);
    }
    #[test]
    fn jit_follows_code_mappings_above_the_first_instruction_leaf() {
        let mut vm = machine(true);
        vm.memory[3074..3076].copy_from_slice(&[7, 0]);
        let entry = 7 * 1024 + 44 * 4; // Virtual bank 300 = directory 1, leaf 44.
        vm.memory[entry..entry + 4].copy_from_slice(&[
            1, 0, PRESENT | READ | EXECUTE | USER, 0,
        ]);
        vm.instructions[1024..1028].copy_from_slice(&[
            i(0x0e, 5, 23, 0),
            i(0x0a, 15, 15, u16::MAX),
            i(0x16, 15, 0, u16::MAX),
            i(0x25, 0, 0, 0),
        ]);
        vm.registers[Register::Pcb as usize] = 300;
        vm.registers[15] = 6000;
        while vm.mmu.user { vm.run_batch(511).unwrap(); }
        assert_eq!(vm.registers[5], 23);
        assert_eq!(vm.mmu.cause, CAUSE_SYSCALL);
        assert!(vm.jit_stats().unwrap().native_instructions > 1000);
    }

    #[test]
    fn native_code_is_reused_only_for_the_matching_instruction_map() {
        let mut vm = machine(true);
        vm.memory[9 * 1024..9 * 1024 + 2].copy_from_slice(&[10, 0]);
        vm.memory[10 * 1024..10 * 1024 + 4].copy_from_slice(&[
            2,
            0,
            PRESENT | READ | EXECUTE | USER,
            0,
        ]);
        for (physical, value) in [(1024, 11), (2048, 22)] {
            vm.instructions[physical..physical + 4].copy_from_slice(&[
                i(0x0e, 5, value, 0),
                i(0x0a, 15, 15, u16::MAX),
                i(0x16, 15, 0, u16::MAX),
                i(0x25, 0, 0, 0),
            ]);
        }
        let mut compiled = 0;
        for (root, value, count) in [(3, 11, 6000), (9, 22, 6000), (3, 11, 10)] {
            if !vm.mmu.user {
                vm.write_csr(CSR_IROOT_BANK, root).unwrap();
                vm.write_csr(CSR_EPC, 0).unwrap();
                vm.interrupt_return().unwrap();
            }
            vm.registers[15] = count;
            while vm.mmu.user {
                vm.run_batch(511).unwrap();
            }
            assert_eq!(vm.registers[5], value);
            assert_eq!(vm.mmu.cause, CAUSE_SYSCALL);
            if count == 10 {
                assert_eq!(vm.jit_stats().unwrap().compiled_regions, compiled);
            }
            compiled = vm.jit_stats().unwrap().compiled_regions;
        }
        assert!(vm.jit_stats().unwrap().native_instructions > 1000);
    }
    #[test]
    fn native_kernel_ram_path_checks_the_current_supervisor_window() {
        let make = |jit| {
            let mut vm = machine(jit);
            vm.mmu.user = false;
            vm.memory[2048 + 8 * 4] = 7;
            vm.memory[6 * 1024 + 7] = 123;
            vm.memory[7 * 1024 + 7] = 456;
            vm.memory[8 * 1024 + 7] = 789;
            vm.write_csr(CSR_WINDOW_LO, 8).unwrap();
            vm.write_csr(CSR_WINDOW_HI, 31).unwrap();
            vm.write_csr(CSR_MMU_CONTROL, 3).unwrap();
            vm.registers[15] = 6000;
            vm.registers[16] = 6;
            vm.registers[17] = 7;
            vm.registers[18] = 8;
            vm.instructions[..5].copy_from_slice(&[
                i(0x11, 5, 16, 17),
                i(0x11, 6, 18, 17),
                i(0x0a, 15, 15, u16::MAX),
                i(0x16, 15, 0, (-3i16) as u16),
                i(0, 0, 0, 0),
            ]);
            vm
        };
        let mut a = make(false);
        let mut b = make(true);
        for pass in 0..2 {
            if pass == 1 {
                for vm in [&mut a, &mut b] {
                    vm.state = VMState::Running;
                    vm.registers[1] = 0;
                    vm.registers[15] = 10;
                    vm.write_csr(CSR_MMU_CONTROL, 1).unwrap();
                }
            }
            while matches!(a.state, VMState::Running) {
                assert_eq!(a.run_batch(511).unwrap(), b.run_batch(511).unwrap());
                assert_eq!(a.registers, b.registers);
            }
            assert_eq!(b.registers[5], 123);
            assert_eq!(b.registers[6], if pass == 0 { 456 } else { 789 });
        }
        assert!(b.jit_stats().unwrap().native_instructions > 1000);
    }
    #[test]
    fn cold_jit_fetch_fault_counts_only_one_instruction_attempt() {
        let make = |jit| {
            let mut vm = machine(jit);
            vm.memory[4096..4100].copy_from_slice(&[
                7,
                0,
                PRESENT | READ | EXECUTE | USER | PAGED,
                0,
            ]);
            vm.memory[7 * 1024..7 * 1024 + 4].copy_from_slice(&[1, 0, PRESENT | EXECUTE | USER, 0]);
            vm.registers[1] = 511;
            vm
        };
        let mut a = make(false);
        let mut b = make(true);
        assert_eq!(a.run_batch(7).unwrap(), 2);
        assert_eq!(b.run_batch(7).unwrap(), 2);
        assert_eq!(a.registers, b.registers);
        assert_eq!(b.mmu.cause, CAUSE_EXECUTE);
        assert_eq!(b.read_csr(CSR_EPC).unwrap(), 512);
    }
    #[test]
    fn loadc_uses_the_instruction_map_and_checks_read_permission() {
        let mut vm = machine(false);
        vm.instructions[1024] = i(0x20, 0, 15, 16);
        vm.instructions[1024 + 7] = i(0x0e, 5, 42, 0);
        vm.registers[15] = 0;
        vm.registers[16] = 7;
        vm.step().unwrap();
        assert_eq!(vm.registers[Register::X2 as usize], 42);
        vm.registers[1] = 0;
        vm.memory[4096 + 2] = PRESENT | EXECUTE | USER;
        vm.mmu.flush();
        vm.step().unwrap();
        assert_eq!(vm.mmu.cause, CAUSE_READ);
        assert_eq!(vm.mmu.address, (0, 7));
        assert_eq!(vm.registers[Register::X2 as usize], 42);
    }
    #[test]
    fn translation_flush_is_explicit_and_zero_is_a_noop() {
        let mut vm = machine(false);
        vm.memory[6 * 1024 + 7] = 123;
        vm.memory[7 * 1024 + 7] = 456;
        assert_eq!(vm.read_cell(8, 7).unwrap(), 123);
        vm.memory[2048 + 8 * 4] = 7;
        vm.write_csr(CSR_MMU_FLUSH, 0).unwrap();
        assert_eq!(vm.read_cell(8, 7).unwrap(), 123);
        vm.write_csr(CSR_MMU_FLUSH, 1).unwrap();
        assert_eq!(vm.read_cell(8, 7).unwrap(), 456);
    }
    #[test]
    fn disabling_translation_cannot_return_into_unprotected_user_mode() {
        let mut vm = machine(false);
        vm.instructions[1024] = i(0x25, 0, 0, 0);
        vm.step().unwrap();
        vm.write_csr(CSR_MMU_CONTROL, 0).unwrap();
        assert!(vm.interrupt_return().is_err());
        assert!(!vm.mmu.user);
    }
}
