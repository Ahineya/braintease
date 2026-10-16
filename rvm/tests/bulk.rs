//! Device-visible transfers, including cache coherence and hot-code replacement.
use rvm::{bulk::*, devices::*, vm::VMState, Instr, VM};
use std::{
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
};
static ID: AtomicU64 = AtomicU64::new(0);
struct Fixture {
    vm: VM,
    path: PathBuf,
}
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "rvm-bulk-{}-{}.img",
            std::process::id(),
            ID.fetch_add(1, Ordering::Relaxed)
        ));
        let mut vm = VM::with_options(65, 65 * 6, Some(path.clone()));
        vm.host_input_enabled = false;
        Self { vm, path }
    }
    fn submit(&mut self, command: u16, d: [u16; 12]) -> u16 {
        self.vm.memory[63..75].copy_from_slice(&d);
        self.vm
            .write_cell(0, REGISTER_DESCRIPTOR_OFFSET as u16, 63)
            .unwrap();
        self.vm
            .write_cell(0, REGISTER_COMMAND as u16, command)
            .unwrap();
        self.vm.read_cell(0, REGISTER_STATUS as u16).unwrap()
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}
fn d(a: u16, b: u16, bank: u16, off: u16, count: u16, format: u16) -> [u16; 12] {
    [1, 12, a, b, bank, off, count, 0, format, 0, 0, 0]
}
#[test]
fn word_transfers_cross_disk_and_non_power_of_two_ram_banks_and_share_cache() {
    let mut f = Fixture::new();
    f.vm.write_cell(0, 17, 7).unwrap();
    f.vm.write_cell(0, 18, 19).unwrap();
    f.vm.memory[127..131].copy_from_slice(&[0xabcd, 0x1234, 0xffff, 0]);
    assert_eq!(
        f.submit(STORAGE_WRITE, d(65534, 0, 1, 62, 4, WORDS)),
        STATUS_OK
    );
    // A positional write neither moves nor selects the legacy port cursor.
    assert_eq!(f.vm.read_cell(0, 19).unwrap(), 0);
    assert_eq!(f.vm.memory[18], 20);
    f.vm.write_cell(0, 17, 0).unwrap();
    f.vm.write_cell(0, 18, 32767).unwrap();
    assert_eq!(f.vm.read_cell(0, 19).unwrap(), 0xabcd);
    f.vm.write_cell(0, 17, 1).unwrap();
    f.vm.write_cell(0, 18, 0).unwrap();
    assert_eq!(f.vm.read_cell(0, 19).unwrap(), 0x1234);
    f.vm.write_cell(0, 19, 0x5678).unwrap();
    assert_eq!(
        f.submit(STORAGE_READ, d(65534, 0, 3, 64, 4, WORDS)),
        STATUS_OK
    );
    assert_eq!(&f.vm.memory[259..263], &[0xabcd, 0x1234, 0x5678, 0]);
    f.vm.write_cell(0, 20, 8).unwrap();
    let bytes = std::fs::read(&f.path).unwrap();
    assert_eq!(
        &bytes[65534..65542],
        &[0xcd, 0xab, 0x34, 0x12, 0x78, 0x56, 0, 0]
    );
}
#[test]
fn octet_transfers_keep_neighbor_octets_and_zero_pad_eof() {
    let mut f = Fixture::new();
    std::fs::write(&f.path, [0x11, 0x22, 0x33]).unwrap();
    assert_eq!(f.submit(STORAGE_READ, d(1, 0, 2, 0, 4, OCTETS)), STATUS_OK);
    assert_eq!(&f.vm.memory[130..134], &[0x22, 0x33, 0, 0]);
    f.vm.memory[130..132].copy_from_slice(&[0xffaa, 0xbb]);
    assert_eq!(f.submit(STORAGE_WRITE, d(1, 0, 2, 0, 2, OCTETS)), STATUS_OK);
    assert_eq!(f.submit(STORAGE_READ, d(0, 0, 3, 0, 2, WORDS)), STATUS_OK);
    assert_eq!(&f.vm.memory[195..197], &[0xaa11, 0xbb]);
    assert_eq!(
        f.submit(STORAGE_READ, d(65535, 65535, 2, 0, 1, OCTETS)),
        STATUS_OK
    );
    assert_eq!(f.vm.memory[130], 0);
    assert_eq!(
        f.submit(STORAGE_READ, d(65535, 65535, 2, 0, 2, OCTETS)),
        STATUS_BOUNDS
    );
}
#[test]
fn invalid_transfers_do_not_mutate_ram_code_or_disk() {
    let mut f = Fixture::new();
    for (command, descriptor, status) in [
        (
            STORAGE_READ,
            {
                let mut v = d(0, 0, 2, 0, 1, WORDS);
                v[0] = 2;
                v
            },
            STATUS_VERSION,
        ),
        (
            STORAGE_READ,
            {
                let mut v = d(0, 0, 2, 0, 1, WORDS);
                v[1] = 11;
                v
            },
            STATUS_BAD_DESCRIPTOR,
        ),
        (
            STORAGE_READ,
            {
                let mut v = d(0, 0, 2, 0, 1, WORDS);
                v[9] = 1;
                v
            },
            STATUS_BAD_DESCRIPTOR,
        ),
        (STORAGE_READ, d(0, 0, 2, 0, 1, 99), STATUS_BAD_DESCRIPTOR),
        (STORAGE_READ, d(0, 0, 0, 31, 2, WORDS), STATUS_BOUNDS),
        (
            STORAGE_READ,
            d(0, 0, 0, 64, 1, WORDS),
            STATUS_BAD_DESCRIPTOR,
        ),
        (STORAGE_READ, d(0, 0, 5, 64, 2, WORDS), STATUS_BOUNDS),
        (STORAGE_READ, d(0, 0, 2, 65, 1, WORDS), STATUS_BOUNDS),
        (STORAGE_READ, d(0, 0, 2, 0, 32769, WORDS), STATUS_BOUNDS),
        (STORAGE_READ, d(0, 0, 2, 0, 8193, CODE_WORDS), STATUS_BOUNDS),
        (
            STORAGE_WRITE,
            d(0, 0, 2, 0, 1, CODE_WORDS),
            STATUS_BAD_DESCRIPTOR,
        ),
        (MEMORY_FILL, d(1, 1, 2, 0, 1, WORDS), STATUS_BAD_DESCRIPTOR),
        (MEMORY_COPY, d(0, 31, 2, 0, 1, WORDS), STATUS_BOUNDS),
        (
            MEMORY_COPY,
            d(2, 0, 65535, 64, 2, CODE_WORDS),
            STATUS_BOUNDS,
        ),
    ] {
        f.vm.memory[63..75].copy_from_slice(&descriptor);
        let before = f.vm.memory.clone();
        assert_eq!(f.submit(command, descriptor), status, "{descriptor:?}");
        assert_eq!(f.vm.memory, before);
        assert!(f.vm.instructions.is_empty());
        assert_eq!(std::fs::metadata(&f.path).unwrap().len(), 0);
    }
}
#[test]
fn memory_copy_is_memmove_and_fill_is_bounded() {
    let mut f = Fixture::new();
    f.vm.memory[130..135].copy_from_slice(&[1, 2, 3, 4, 5]);
    assert_eq!(f.submit(MEMORY_COPY, d(2, 0, 2, 1, 4, WORDS)), STATUS_OK);
    assert_eq!(&f.vm.memory[130..135], &[1, 1, 2, 3, 4]);
    assert_eq!(
        f.submit(MEMORY_FILL, d(0xabcd, 0, 2, 63, 4, WORDS)),
        STATUS_OK
    );
    assert_eq!(&f.vm.memory[193..197], &[0xabcd; 4]);
    let before = f.vm.memory.clone();
    assert_eq!(f.submit(MEMORY_FILL, d(1, 0, 2, 0, 0, WORDS)), STATUS_OK);
    assert_eq!(&f.vm.memory[130..], &before[130..]);
}
#[test]
fn code_upload_decodes_octets_grows_with_nops_and_preserves_registers() {
    let mut f = Fixture::new();
    let words = [0xff0e, 7, 0x1234, 0, 0, 0, 0, 0];
    f.vm.memory[130..138].copy_from_slice(&words);
    let registers = f.vm.registers;
    assert_eq!(
        f.submit(MEMORY_COPY, d(2, 0, 1, 64, 2, CODE_WORDS)),
        STATUS_OK
    );
    assert_eq!(f.vm.instructions.len(), 195);
    assert!(f.vm.instructions[..129].iter().all(|i| i.opcode == 0x42));
    assert_eq!(f.vm.instructions[129].opcode, 14);
    assert_eq!(f.vm.registers, registers);
    let bytes = [14, 0xaa, 7, 0, 0x78, 0x56, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    f.vm.memory[130..146].copy_from_slice(&bytes);
    assert_eq!(
        f.submit(MEMORY_COPY, d(2, 0, 1, 64, 2, CODE_OCTETS)),
        STATUS_OK
    );
    assert_eq!(f.vm.instructions[129].word2, 0x5678);
    f.vm.registers[1] = 64;
    f.vm.registers[2] = 1;
    f.vm.state = VMState::Running;
    f.vm.run_batch(10).unwrap();
    assert_eq!(f.vm.registers[7], 0x5678);
    assert!(matches!(f.vm.state, VMState::Halted));
}
#[test]
fn disk_to_code_executes_and_bulk_replacement_invalidates_hot_jit_regions() {
    for jit in [false, true] {
        let mut f = Fixture::new();
        f.vm.jit_enabled = jit;
        std::fs::write(&f.path, [14, 0, 7, 0, 1, 0, 0, 0]).unwrap();
        assert_eq!(
            f.submit(STORAGE_READ, d(0, 0, 0, 0, 1, CODE_WORDS)),
            STATUS_OK
        );
        f.vm.instructions[1] = Instr::new(10, 10, 15, 15, 65535);
        f.vm.instructions[2] = Instr::new(22, 22, 15, 0, 65534);
        f.vm.instructions[3] = Instr::new(0, 0, 0, 0, 0);
        f.vm.registers[15] = 5000;
        f.vm.state = VMState::Running;
        while matches!(f.vm.state, VMState::Running) {
            f.vm.run_batch(4096).unwrap();
        }
        assert_eq!(f.vm.registers[7], 1);
        if jit {
            assert!(f.vm.jit_stats().unwrap().compiled_regions > 0);
        }
        f.vm.memory[130..134].copy_from_slice(&[14, 7, 7, 0]);
        assert_eq!(
            f.submit(MEMORY_COPY, d(2, 0, 0, 0, 1, CODE_WORDS)),
            STATUS_OK
        );
        f.vm.registers[1] = 0;
        f.vm.registers[2] = 0;
        f.vm.registers[15] = 5000;
        f.vm.state = VMState::Running;
        while matches!(f.vm.state, VMState::Running) {
            f.vm.run_batch(4096).unwrap();
        }
        assert_eq!(f.vm.registers[7], 7);
    }
}
#[test]
fn missing_backends_report_unsupported_without_consuming_the_descriptor() {
    let mut ram = vec![0; 260];
    let mut state = DeviceState::default();
    ram[40..52].copy_from_slice(&d(0, 0, 2, 0, 1, WORDS));
    let before = ram.clone();
    state.write_register(REGISTER_DESCRIPTOR_OFFSET, 40, &mut ram, 65, 0);
    state.write_register(REGISTER_COMMAND, STORAGE_READ, &mut ram, 65, 0);
    assert_eq!(state.status(), STATUS_UNSUPPORTED);
    assert_eq!(ram, before);
    ram[48] = CODE_WORDS;
    let before = ram.clone();
    state.write_register(REGISTER_COMMAND, MEMORY_COPY, &mut ram, 65, 0);
    assert_eq!(state.status(), STATUS_UNSUPPORTED);
    assert_eq!(ram, before);
}
