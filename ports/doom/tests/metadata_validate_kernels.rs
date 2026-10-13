//! Execute the port's actual guest validator against an independent u32 oracle.
//! No compiled C caller, WAD decoder, or host replacement for the assembly.
use rvm::{vm::VMState, Instr, VM};
use std::{fs, path::{Path, PathBuf}, time::{Duration, Instant}};

const BANK: usize = 64_000;
const RECORD_WORDS: usize = 18;
const RAM_START: usize = 32; // The first cells are device/debugger mirrors.
const STEP_LIMIT: u64 = 10_000;
const CASE_TIMEOUT: Duration = Duration::from_secs(5);

struct Kernel {
    vm: VM,
    entry: usize,
    halt: usize,
}

impl Kernel {
    fn new(binary: &[u8], disk: &Path) -> Self {
        let mut vm = VM::with_options(BANK as u16, BANK * 10, Some(disk.to_owned()));
        vm.host_input_enabled = false;
        vm.load_binary(binary).expect("load assembled metadata validator");
        assert_eq!(vm.bank_size as usize, BANK, "kernel requires 64000-cell banks");
        let entry = *vm.debug_symbols.iter()
            .find(|(_, name)| name.as_str() == "Doom_ValidateMetadata")
            .expect("missing Doom_ValidateMetadata symbol").0;
        // Return to another code bank to check both RA and RAB independently.
        let halt = vm.instructions.len().max(BANK + 17);
        vm.instructions.resize(halt + 1, Instr {
            opcode: 0, word0: 0, word1: 0, word2: 0, word3: 0,
        });
        for (address, cell) in vm.memory.iter_mut().enumerate() {
            *cell = (address.wrapping_mul(31) ^ 0xdead) as u16;
        }
        Self { vm, entry, halt }
    }

    fn run(&mut self, label: &str, pointer: (u16, u16), count: u16,
           first: u16, length: u32, records: &[u16]) -> String {
        self.vm.registers[23..27].copy_from_slice(&[0xace1, 0x9876, 0xcdef, 0xfedc]);
        self.vm.registers[28] = 5; // SB
        self.vm.registers[29] = 1000; // SP
        self.vm.registers[30] = 777; // FP
        self.vm.registers[31] = 3; // GP
        let bank = match pointer.1 {
            65535 => 3,
            65534 => 5,
            other => other as usize,
        };
        let pointer_valid = (pointer.0 as usize) < BANK && bank != 65533;
        let expected = oracle(pointer_valid, count, first, length, records);
        if pointer_valid && count != 0 && count <= 16
            && u32::from(first) + u32::from(count) <= 32767 {
            let start = bank * BANK + pointer.0 as usize;
            let end = start + records.len();
            assert!(start >= RAM_START, "case input must avoid MMIO");
            assert!(end <= self.vm.memory.len(), "case input fits RAM");
            // Keep stack arguments separate from tagged stack input.
            assert!(end <= 5 * BANK + 998 || start >= 5 * BANK + 1000,
                    "case input does not overlap arguments");
            self.vm.memory[start..end].copy_from_slice(records);
        }
        self.vm.memory[5 * BANK + 999] = length as u16;
        self.vm.memory[5 * BANK + 998] = (length >> 16) as u16;
        let memory_before = self.vm.memory[RAM_START..].to_vec();
        self.vm.registers[1] = (self.entry % BANK) as u16;
        self.vm.registers[2] = (self.entry / BANK) as u16;
        self.vm.registers[3] = (self.halt % BANK) as u16;
        self.vm.registers[4] = (self.halt / BANK) as u16;
        self.vm.registers[5] = 0xaaaa;
        self.vm.registers[6] = 0xbbbb;
        self.vm.registers[7] = pointer.0;
        self.vm.registers[8] = pointer.1;
        self.vm.registers[9] = count;
        self.vm.registers[10] = first;
        let saved: Vec<_> = [3, 4, 23, 24, 25, 26, 28, 29, 30, 31]
            .into_iter().map(|reg| (reg, self.vm.registers[reg])).collect();
        self.vm.state = VMState::Running;
        let start = Instant::now();
        let before = self.vm.executed_instructions;
        while matches!(self.vm.state, VMState::Running) {
            self.vm.step().unwrap_or_else(|error| panic!("{label}: kernel execution: {error:?}"));
            let steps = self.vm.executed_instructions - before;
            assert!(steps <= STEP_LIMIT, "{label}: instruction budget");
            if steps % 1024 == 0 {
                assert!(start.elapsed() < CASE_TIMEOUT, "{label}: wall timeout");
            }
        }
        assert!(matches!(self.vm.state, VMState::Halted), "{label}: returns to HALT");
        assert_eq!((self.vm.registers[1], self.vm.registers[2]),
            (((self.halt + 1) % BANK) as u16, ((self.halt + 1) / BANK) as u16),
            "{label}: return code bank and offset");
        assert!(start.elapsed() < CASE_TIMEOUT, "{label}: wall timeout");
        for (reg, value) in saved {
            assert_eq!(self.vm.registers[reg], value, "{label}: preserved register {reg}");
        }
        assert_eq!(&self.vm.memory[RAM_START..], memory_before.as_slice(),
                   "{label}: validator is read-only, including input and all RAM guards");
        assert_eq!((self.vm.registers[5], self.vm.registers[6]), expected,
                   "{label}: error code and absolute record index");
        let steps = self.vm.executed_instructions - before;
        // A full batch should remain a small guest leaf, never a software
        // division or generic far-pointer helper per field.
        if expected == (0, 0) && count == 16 {
            assert!(steps < 2800, "{label}: validation hot path regressed");
        }
        format!("{{\"case\":\"{label}\",\"bank\":{},\"offset\":{},\"count\":{count},\"first\":{first},\"file_length\":{length},\"code\":{},\"index\":{},\"instructions\":{steps}}}",
                pointer.1, pointer.0, expected.0, expected.1)
    }
}

fn oracle(pointer_valid: bool, count: u16, first: u16, length: u32,
          records: &[u16]) -> (u16, u16) {
    if count == 0 { return (0, 0); }
    if count > 16 || u32::from(first) + u32::from(count) > 32767 { return (4, first); }
    if !pointer_valid { return (5, first); }
    assert_eq!(records.len(), count as usize * RECORD_WORDS);
    for (i, record) in records.chunks_exact(RECORD_WORDS).enumerate() {
        let index = first + i as u16;
        if record[..8].iter().any(|&word| word > 255) { return (1, index); }
        if record[8] != 0 || record[9] != 0 { return (2, index); }
        let position = u32::from(record[10]) + u32::from(record[11]) * 65536;
        let size = u32::from(record[12]) + u32::from(record[13]) * 65536;
        if position > length || size > length - position { return (3, index); }
        if record[14] != 0 || record[15] != 0 || record[17] != 0
            || (record[16] != 65535 && record[16] >= index) { return (2, index); }
    }
    (0, 0)
}

fn records(count: u16, first: u16, position: u32, size: u32) -> Vec<u16> {
    let mut result = vec![0; count as usize * RECORD_WORDS];
    for (i, record) in result.chunks_exact_mut(RECORD_WORDS).enumerate() {
        record[..8].copy_from_slice(&[0, 1, 65, 90, 127, 128, 254, 255]);
        record[10] = position as u16;
        record[11] = (position >> 16) as u16;
        record[12] = size as u16;
        record[13] = (size >> 16) as u16;
        record[16] = if i == 0 { 65535 } else { first + i as u16 - 1 };
    }
    result
}

fn main() {
    let root = PathBuf::from(std::env::args().nth(1)
        .expect("usage: metadata_validate_kernels OUTPUT_DIRECTORY"));
    let binary = fs::read(root.join("metadata_validate_test.bin"))
        .expect("read assembled metadata validator");
    // Use an isolated disk even though this kernel performs no device I/O;
    // passing None would initialize the VM's ordinary default storage path.
    let disk = root.join("metadata-validate-kernel.disk");
    let disk_bytes = vec![0u8; 65536];
    fs::write(&disk, &disk_bytes).expect("write isolated kernel disk");
    let mut kernel = Kernel::new(&binary, &disk);
    let mut reports = Vec::new();

    // Every place a record may straddle a bank, on canonical and both tagged
    // pointers. Batch lengths also cover exact bank endings and partial batches.
    for bank in [7, 65535, 65534] {
        for count in [1, 2, 16] {
            let mut offsets = vec![123, BANK as u16 - count * RECORD_WORDS as u16];
            offsets.extend((1..=RECORD_WORDS).map(|n| (BANK - n) as u16));
            offsets.sort_unstable();
            offsets.dedup();
            for offset in offsets {
                let input = records(count, 100, 65535, 2);
                reports.push(kernel.run(&format!("cross_{bank}_{count}_{offset}"),
                    (offset, bank), count, 100, 65537, &input));
            }
        }
    }
    reports.push(kernel.run("physical_bank_zero", (128, 0), 1, 0, 0,
                            &records(1, 0, 0, 0)));

    // Every serialized cell gets an independently rejected mutation, including
    // the first, middle, and final records of a full batch. Names after a NUL
    // must still be octets, and high bits must not pass signed comparisons.
    for row in [0usize, 7, 15] {
        for field in 0..RECORD_WORDS {
            for high in [false, true] {
                let mut input = records(16, 100, 0, 0);
                input[row * RECORD_WORDS + field] = match field {
                    0..=7 => if high { 65535 } else { 256 },
                    16 => if high { 32767 } else { 100 + row as u16 },
                    _ => if high { 65535 } else { 1 },
                };
                let label = format!("field_{row}_{field}_{}", high as u8);
                reports.push(kernel.run(&label, (63991, 65534), 16, 100, 0, &input));
            }
        }
    }

    // Independent full-width arithmetic cases: exact EOF, empty lumps, low
    // word borrow, sign boundaries, UINT32_MAX, and overflowing position+size.
    for length in [0u32, 1, 65535, 65536, 65537, 0x7fff_ffff,
                   0x8000_0000, 0x8000_ffff, 0xffff_0000, u32::MAX] {
        let mut positions = vec![0, 1, 65535, 65536, length,
                                 length.saturating_sub(1), length.saturating_add(1), u32::MAX];
        positions.sort_unstable();
        positions.dedup();
        for position in positions {
            let remaining = length.saturating_sub(position);
            let mut sizes = vec![0, 1, 65535, 65536, remaining,
                                 remaining.saturating_add(1), u32::MAX];
            sizes.sort_unstable();
            sizes.dedup();
            for size in sizes {
                let label = format!("range_{length}_{position}_{size}");
                let input = records(1, 321, position, size);
                reports.push(kernel.run(&label, (63989, 7), 1, 321, length, &input));
            }
        }
    }

    for (next, first) in [(65535, 0), (0, 0), (0, 1), (99, 100), (100, 100),
                          (101, 100), (32765, 32766), (32766, 32766), (65534, 100)] {
        let mut input = records(1, first, 0, 0);
        input[16] = next;
        reports.push(kernel.run(&format!("next_{first}_{next}"),
                                (63983, 65535), 1, first, 0, &input));
    }
    for (count, first) in [(1, 32766), (16, 32751), (1, 32767), (16, 32752),
                           (1, 65535), (16, 65530), (17, 0), (65535, 1)] {
        let input = if count <= 16 && u32::from(count) + u32::from(first) <= 32767 {
            records(count, first, 0, 0)
        } else { Vec::new() };
        reports.push(kernel.run(&format!("count_{count}_{first}"),
                                (400, 7), count, first, 0, &input));
    }
    for pointer in [(0, 65533), (1, 65533), (64000, 7), (65535, 7)] {
        reports.push(kernel.run(&format!("bad_pointer_{}_{}", pointer.0, pointer.1),
                                pointer, 1, 100, 0, &[]));
    }
    // These pointers would fail or read devices if dereferenced. A zero count
    // must return exactly zero without inspecting pointer or stack arguments.
    for pointer in [(0, 65533), (65535, 65533), (0, 0), (65535, 7),
                    (0, 65535), (0, 65534)] {
        reports.push(kernel.run(&format!("zero_{}_{}", pointer.0, pointer.1),
                                pointer, 0, 65535, u32::MAX, &[]));
    }
    assert_eq!(fs::read(&disk).expect("reread isolated kernel disk"), disk_bytes,
               "validator leaves backing disk unchanged");
    let report = format!("{{\"success\":true,\"bank_size\":64000,\"cases\":[{}]}}\n",
                         reports.join(","));
    fs::write(root.join("metadata-validate-kernel-results.json"), report)
        .expect("write metadata validator report");
    println!("Metadata validator: {} cases passed (all 18 cells, u32 bounds, links, bank crossings, pointer tags, zero/count limits, full RAM guards, error indices and saved registers)", reports.len());
}
