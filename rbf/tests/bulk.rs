//! Real BF CPU submits transfers, then executes code loaded beyond its RAM.
use bf_wasm::engine::Program;
use rbf::{
    devices::BfDevices,
    image::{Layout, ORIGIN},
    runner::{Host, RunOptions},
};
use ripple_asm::{AssemblerOptions, Linker, MacroFormatter, RippleAssembler};
use rvm::{bulk::*, vm::Storage};
use std::{
    sync::atomic::{AtomicU64, Ordering},
    time::Duration,
};
static ID: AtomicU64 = AtomicU64::new(0);
fn store(source: &mut String, index: usize, value: u16) {
    source.push_str(&format!(
        "LI T0, {}\nLI T1, {}\nLI T2, {value}\nSTORE T2, T0, T1\n",
        index / 513,
        index % 513
    ));
}
fn command(source: &mut String, command: u16, d: [u16; 12], result: usize) {
    for (i, v) in d.into_iter().enumerate() {
        store(source, 63 + i, v);
    }
    store(source, 28, 63);
    store(source, 29, command);
    source.push_str(&format!(
        "LI T0, 30\nLOAD S0, R0, T0\nLI T0, {}\nLI T1, {}\nSTORE S0, T0, T1\n",
        result / 513,
        result % 513
    ));
}
#[test]
fn transfers_and_loaded_code_work_in_both_bf_engines() {
    for disk_code in [false, true] {
        let mut source = String::from("start:\nLI X0, 111\nLI X1, 222\nLI X2, 333\nLI X3, 444\n");
        command(
            &mut source,
            STORAGE_READ,
            [1, 12, 65534, 0, 2, 511, 4, 0, WORDS, 0, 0, 0],
            450,
        );
        command(
            &mut source,
            STORAGE_WRITE,
            [1, 12, 1, 2, 2, 511, 4, 0, WORDS, 0, 0, 0],
            451,
        );
        command(
            &mut source,
            MEMORY_FILL,
            [1, 12, 0xabcd, 0, 4, 512, 3, 0, WORDS, 0, 0, 0],
            452,
        );
        command(
            &mut source,
            MEMORY_COPY,
            [1, 12, 2, 511, 6, 512, 4, 0, WORDS, 0, 0, 0],
            453,
        );
        if disk_code {
            command(
                &mut source,
                STORAGE_READ,
                [1, 12, 16, 0, 67, 512, 2, 0, CODE_WORDS, 0, 0, 0],
                454,
            );
        } else {
            let bytes = [14, 0xff, 7, 0, 65, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
            for (i, v) in bytes.into_iter().enumerate() {
                store(&mut source, 2052 + i, v);
            }
            command(
                &mut source,
                MEMORY_COPY,
                [1, 12, 4, 0, 67, 512, 2, 0, CODE_OCTETS, 0, 0, 0],
                454,
            );
        }
        source.push_str("LI T0, 67\nLI T1, 512\nJALR R0, T0, T1\n");
        let object = RippleAssembler::new(AssemblerOptions {
            bank_size: 513,
            max_immediate: 65535,
            ..Default::default()
        })
        .assemble(&source)
        .unwrap();
        let image = Linker::new(513).link(vec![object]).unwrap();
        let mut bytes = Vec::new();
        MacroFormatter::new()
            .with_code_banks(69)
            .write_linked_program_host_bf(&image, 8, &mut bytes)
            .unwrap();
        let source = String::from_utf8(bytes).unwrap();
        let (layout, body) = Layout::parse(&source, 2000000).unwrap();
        let program = Program::parse(body, true, true).unwrap();
        for jit in [false, true] {
            let path = std::env::temp_dir().join(format!(
                "rbf-bulk-{}-{}.img",
                std::process::id(),
                ID.fetch_add(1, Ordering::Relaxed)
            ));
            let mut disk = vec![0; 65542];
            disk[16..32].copy_from_slice(&[14, 0xff, 7, 0, 65, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
            disk[65534..65542].copy_from_slice(&[0xcd, 0xab, 0x34, 0x12, 0xff, 0xff, 0, 0]);
            std::fs::write(&path, &disk).unwrap();
            let mut devices =
                BfDevices::new(513, 35, 1, Some(Storage::with_path(path.clone()).unwrap()))
                    .unwrap();
            devices.host_input_enabled = false;
            let mut host = Host::new(layout.clone(), devices).unwrap();
            assert_eq!(
                host.run(
                    &program,
                    &RunOptions {
                        jit,
                        max_instructions: Some(1000),
                        timeout: Some(Duration::from_secs(40)),
                        ..Default::default()
                    },
                    &mut Vec::new()
                )
                .unwrap(),
                "halted"
            );
            let ram = |i: usize| host.machine.read_cell(ORIGIN + 8 * i + 4);
            for i in 450..455 {
                assert_eq!(
                    ram(i),
                    0,
                    "command {} disk_code={disk_code} jit={jit}",
                    i - 450
                );
            }
            assert_eq!(
                (1537..1541).map(ram).collect::<Vec<_>>(),
                [0xabcd, 0x1234, 0xffff, 0]
            );
            assert_eq!(
                (3590..3594).map(ram).collect::<Vec<_>>(),
                [0xabcd, 0x1234, 0xffff, 0]
            ); // bank-crossing RAM copy
            for i in 2564..2567 {
                assert_eq!(ram(i), 0xabcd);
            }
            assert_eq!(host.machine.read_cell(ORIGIN + 8 * 8 + 1), 65);
            for (reg, v) in [(11, 111), (12, 222), (13, 333), (14, 444)] {
                assert_eq!(host.machine.read_cell(ORIGIN + 8 * (reg + 1) + 1), v);
            }
            host.devices.flush_storage().unwrap();
            drop(host);
            assert_eq!(
                &std::fs::read(&path).unwrap()[131073..131081],
                &disk[65534..65542]
            );
            std::fs::remove_file(path).unwrap();
        }
    }
}

#[test]
fn code_adapter_preserves_every_other_lane_and_rejects_invalid_capacity() {
    use bf_wasm::engine::Machine;
    use rbf::image::{MachineMemory, StridedMemory};
    use ripple_asm::bf_cpu::*;
    use rvm::devices::GuestMemory;
    let cpu = |word: usize| ORIGIN + word * 8 + 1;
    let code_cell = |index: usize, word: usize| ORIGIN + (37 + index * 4 + word) * 8 + 3;
    let mut tape = vec![7; 8000];
    for (word, value) in [
        (MAGIC_WORD, MAGIC),
        (CAPACITY_BANK_WORD, 3),
        (CAPACITY_OFF_WORD, 0),
        (CAPACITY_FULL_WORD, 0),
        (EXTENT_BANK_WORD, 0),
        (EXTENT_OFF_WORD, 2),
        (EXTENT_FULL_WORD, 0),
    ] {
        tape[cpu(word)] = value;
    }
    let before = tape.clone();
    let mut machine = Machine::new(tape.len()).unwrap();
    machine.tape_mut().copy_from_slice(&tape);
    let instructions = [[0xff0e, 7, 0x1234, 0], [0, 0, 0, 0]];
    {
        let mut a = StridedMemory::new(&mut tape, 260);
        let mut b = MachineMemory::new(&mut machine, 260);
        assert_eq!(a.code_capacity(65), 195);
        assert_eq!(b.code_capacity(65), 195);
        a.write_code(65, 129, &instructions).unwrap();
        b.write_code(65, 129, &instructions).unwrap();
    }
    assert_eq!(tape, machine.tape());
    for (i, (&old, &new)) in before.iter().zip(&tape).enumerate() {
        let grown_code = i >= code_cell(2, 0) && i <= code_cell(194, 3) && (i - ORIGIN) % 8 == 3;
        if !grown_code
            && ![
                cpu(EXTENT_BANK_WORD),
                cpu(EXTENT_OFF_WORD),
                cpu(EXTENT_FULL_WORD),
            ]
            .contains(&i)
        {
            assert_eq!(new, old, "unrelated tape cell {i}");
        }
    }
    assert_eq!(tape[code_cell(128, 3)], 0x42);
    assert_eq!(tape[code_cell(129, 3)], 14);
    assert_eq!(tape[code_cell(129, 1)], 0x1234);
    let before = tape.clone();
    assert!(StridedMemory::new(&mut tape, 260)
        .write_code(65, 195, &instructions)
        .is_err());
    assert_eq!(tape, before);
    tape[cpu(CAPACITY_BANK_WORD)] = 65535;
    assert_eq!(StridedMemory::new(&mut tape, 260).code_capacity(65), 0);
    tape[cpu(CAPACITY_BANK_WORD)] = 0; // Old image: no published capacity.
    assert_eq!(StridedMemory::new(&mut tape, 260).code_capacity(65), 0);
}
