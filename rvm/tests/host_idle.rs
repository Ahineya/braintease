//! The bounded idle command changes host scheduling, never logical guest state.
use rvm::devices::*;
use std::time::{Duration, Instant};

fn request(state: &mut DeviceState, memory: &mut [u16], command: u16) {
    state.write_register(REGISTER_DESCRIPTOR_OFFSET, 63, memory, 64, 0);
    state.write_register(REGISTER_COMMAND, command, memory, 64, 0);
}

#[test]
fn idle_validates_before_waiting_and_never_modifies_guest_memory_or_ticks() {
    let mut state = DeviceState::default();
    let mut memory = vec![0xa55a; 256];
    for (version, length, ms, reserved, status) in [
        (1, 4, 0, 0, STATUS_OK),
        (1, 4, 2, 0, STATUS_OK),
        (1, 4, 10, 0, STATUS_OK),
        (1, 4, 11, 0, STATUS_BAD_DESCRIPTOR),
        (1, 4, 65535, 0, STATUS_BAD_DESCRIPTOR),
        (1, 4, 1, 1, STATUS_BAD_DESCRIPTOR),
        (2, 4, 1, 0, STATUS_VERSION),
        (1, 3, 1, 0, STATUS_BAD_DESCRIPTOR),
    ] {
        memory[63..67].copy_from_slice(&[version, length, ms, reserved]);
        let before = memory.clone();
        let start = Instant::now();
        request(&mut state, &mut memory, COMMAND_HOST_IDLE);
        if status == STATUS_OK && ms != 0 {
            assert!(start.elapsed() >= Duration::from_millis(u64::from(ms)));
        }
        assert_eq!(state.status(), status);
        assert_eq!(memory, before);
        assert_eq!(state.tick(), 0);
        assert!(!state.has_pending_tick());
    }
    memory[63..67].copy_from_slice(&[1, 4, 0, 0]);
    request(&mut state, &mut memory, COMMAND_CAPABILITIES);
    assert_ne!(memory[65] & CAP_HOST_IDLE, 0);
}

#[test]
fn idle_preserves_pending_ticks_and_input_events() {
    let mut state = DeviceState::default();
    let mut memory = vec![0u16; 256];
    state.accept_input([KeyTransition { key: 4, down: true }]);
    memory[63..67].copy_from_slice(&[1, 4, 0, 0]);
    request(&mut state, &mut memory, COMMAND_HOST_IDLE);
    assert!(state.held(4));
    memory[63..73].copy_from_slice(&[1, 10, 1, 2, 0, 0, 0, 0, 0, 0]);
    request(&mut state, &mut memory, COMMAND_INPUT_READ);
    assert_eq!(state.status(), STATUS_OK);
    assert_eq!(memory[68], 1);
    assert_eq!(memory[130], 4);
    memory[63..71].copy_from_slice(&[1, 8, 0, 0, 0, 0, 0, 0]);
    request(&mut state, &mut memory, COMMAND_NEXT_TICK);
    let before = memory.clone();
    request(&mut state, &mut memory, COMMAND_HOST_IDLE);
    assert_eq!(state.status(), STATUS_PENDING);
    assert_eq!(memory, before);
    assert_eq!(state.tick(), 0);
    state.complete_tick(&mut memory, 64, &[]).unwrap();
    assert_eq!(state.tick(), 1);
    assert!(state.held(4));
}

#[test]
fn idle_uses_logical_memory_on_a_strided_bf_adapter() {
    struct Tape(Vec<u16>);
    impl GuestMemory for Tape {
        fn len(&self) -> usize {
            self.0.len() / 8
        }
        fn read(&self, i: usize) -> u16 {
            self.0[i * 8 + 4]
        }
        fn write(&mut self, _i: usize, _n: u16) {
            panic!("idle must not dirty guest RAM or COW pages");
        }
    }
    let mut tape = Tape(vec![0xa55a; 128 * 8]);
    for (i, n) in [1, 4, 0, 0].into_iter().enumerate() {
        tape.0[(63 + i) * 8 + 4] = n;
    }
    let before = tape.0.clone();
    let mut state = DeviceState::default();
    state.write_register_with_memory(REGISTER_DESCRIPTOR_OFFSET, 63, &mut tape, 64, 0);
    state.write_register_with_memory(REGISTER_COMMAND, COMMAND_HOST_IDLE, &mut tape, 64, 0);
    assert_eq!(state.status(), STATUS_OK);
    assert_eq!(tape.0, before);
    assert_eq!(state.tick(), 0);
}

#[test]
fn vm_interpreter_and_jit_execute_idle_between_monotonic_reads() {
    use ripple_asm::{AssemblerOptions, Linker, RippleAssembler};
    use rvm::{vm::VMState, Instr, VM};
    let source = format!("start:\nLI T0, 28\nLI T1, 63\nSTORE T1, R0, T0\nLI T0, 29\nLI T1, {COMMAND_HOST_MONOTONIC}\nSTORE T1, R0, T0\nLI S0, 4096\nL_loop:\nADDI S0, S0, -1\nBNE S0, R0, L_loop\nLI T0, 27\nLI T1, 1\nSTORE T1, R0, T0\nLI T0, 28\nLI T1, 16\nSTORE T1, R0, T0\nLI T0, 29\nLI T1, {COMMAND_HOST_IDLE}\nSTORE T1, R0, T0\nLI T0, 30\nLOAD RV0, R0, T0\nLI T0, 28\nLI T1, 26\nSTORE T1, R0, T0\nLI T0, 29\nLI T1, {COMMAND_HOST_MONOTONIC}\nSTORE T1, R0, T0\nHALT\n");
    let object = RippleAssembler::new(AssemblerOptions {
        bank_size: 64,
        ..Default::default()
    })
    .assemble(&source)
    .unwrap();
    let image = Linker::new(64).link(vec![object]).unwrap();
    for jit in [false, true] {
        let mut vm = VM::with_options(64, 256, None);
        vm.host_input_enabled = false;
        vm.jit_enabled = jit;
        for (offset, words) in [
            (63, vec![1, 10, 0, 0, 0, 0, 0, 0, 0, 0]),
            (80, vec![1, 4, 2, 0]),
            (90, vec![1, 10, 0, 0, 0, 0, 0, 0, 0, 0]),
        ] {
            vm.memory[offset..offset + words.len()].copy_from_slice(&words);
        }
        vm.instructions = image
            .instructions
            .iter()
            .map(|i| Instr::new(i.opcode, i.word0, i.word1, i.word2, i.word3))
            .collect();
        vm.state = VMState::Running;
        for _ in 0..32 {
            if !matches!(vm.state, VMState::Running) {
                break;
            }
            vm.run_batch(4096).unwrap();
        }
        assert!(matches!(vm.state, VMState::Halted));
        assert_eq!(vm.registers[5], STATUS_OK);
        assert_eq!(vm.devices.status(), STATUS_OK);
        assert_eq!(vm.devices.tick(), 0);
        let nanos = u32::from(vm.memory[96]) | u32::from(vm.memory[97]) << 16;
        assert!(nanos >= 2_000_000 || vm.memory[92..96].iter().any(|&n| n != 0));
        if jit {
            assert!(vm.jit_stats().unwrap().compiled_regions > 0);
        }
    }
}
