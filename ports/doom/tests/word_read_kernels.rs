//! Execute the actual port assembly against an independent disk-word oracle.
//! No compiled C caller, WAD decoder, or host replacement for the copy loop.
use rvm::{vm::VMState, Instr, VM};
use std::{fs, path::Path, time::{Duration, Instant}};

const BANK: usize = 64_000;
const BLOCK_WORDS: usize = 32_768;
const RAM_START: usize = 32; // Exclude the device register header only.
const STEP_LIMIT: u64 = 5_000_000;
const CASE_TIMEOUT: Duration = Duration::from_secs(10);

struct Kernel {
    vm: VM,
    entry: usize,
    halt: usize,
}

impl Kernel {
    fn new(binary: &[u8], disk: &Path) -> Self {
        let mut vm = VM::with_options(BANK as u16, BANK * 16, Some(disk.to_owned()));
        vm.host_input_enabled = false;
        vm.load_binary(binary).expect("load assembled word reader");
        assert_eq!(vm.bank_size as usize, BANK, "kernel requires 64000-cell banks");
        let entry = *vm.debug_symbols.iter()
            .find(|(_, name)| name.as_str() == "Doom_ReadStorageWords")
            .expect("missing Doom_ReadStorageWords symbol").0;
        // Return to another code bank, independently checking both RA and RAB.
        let halt = vm.instructions.len().max(BANK + 17);
        vm.instructions.resize(halt + 1, Instr {
            opcode: 0, word0: 0, word1: 0, word2: 0, word3: 0,
        });
        for (address, cell) in vm.memory.iter_mut().enumerate() {
            *cell = (address.wrapping_mul(31) ^ 0xdead) as u16;
        }
        vm.registers[23..27].copy_from_slice(&[0xace1, 0x9876, 0xcdef, 0xfedc]);
        vm.registers[28] = 5; // SB
        vm.registers[29] = 1000; // SP
        vm.registers[30] = 777; // FP
        vm.registers[31] = 3; // GP
        Self { vm, entry, halt }
    }

    fn physical(&self, (offset, bank): (u16, u16)) -> usize {
        assert!((offset as usize) < BANK, "canonical offset precondition");
        let bank = match bank {
            65535 => self.vm.registers[31],
            65534 => self.vm.registers[28],
            value => value,
        };
        assert_ne!(bank, 0, "test destination avoids MMIO");
        bank as usize * BANK + offset as usize
    }

    fn copy(&mut self, dst: (u16, u16), source: u16, count: u16, words: &[u16]) -> u64 {
        assert!(count == 0 || source as usize + count as usize <= BLOCK_WORDS);
        let dest = self.physical(dst);
        let end = dest + count as usize;
        let mut expected = self.vm.memory[RAM_START..].to_vec();
        if count != 0 {
            expected[dest - RAM_START..end - RAM_START]
                .copy_from_slice(&words[source as usize..source as usize + count as usize]);
        }
        self.vm.write_cell(0, 17, 0).expect("select disk block");
        self.vm.write_cell(0, 18, source).expect("select disk word");
        self.vm.registers[1] = (self.entry % BANK) as u16;
        self.vm.registers[2] = (self.entry / BANK) as u16;
        self.vm.registers[3] = (self.halt % BANK) as u16;
        self.vm.registers[4] = (self.halt / BANK) as u16;
        self.vm.registers[5] = 0xaaaa;
        self.vm.registers[6] = 0xbbbb;
        self.vm.registers[7] = dst.0;
        self.vm.registers[8] = dst.1;
        self.vm.registers[9] = count;
        let saved: Vec<_> = [3, 4, 23, 24, 25, 26, 28, 29, 30, 31]
            .into_iter().map(|reg| (reg, self.vm.registers[reg])).collect();
        self.vm.state = VMState::Running;
        let start = Instant::now();
        let before = self.vm.executed_instructions;
        while matches!(self.vm.state, VMState::Running) {
            self.vm.step().expect("word reader execution");
            let steps = self.vm.executed_instructions - before;
            assert!(steps <= STEP_LIMIT, "word reader instruction budget");
            if steps % 4096 == 0 {
                assert!(start.elapsed() < CASE_TIMEOUT, "word reader wall timeout");
            }
        }
        assert!(matches!(self.vm.state, VMState::Halted), "kernel must return to HALT");
        assert_eq!((self.vm.registers[1], self.vm.registers[2]),
            (((self.halt + 1) % BANK) as u16, ((self.halt + 1) / BANK) as u16),
            "returned to the requested code bank and offset");
        assert!(start.elapsed() < CASE_TIMEOUT, "word reader wall timeout");
        for (reg, value) in saved {
            assert_eq!(self.vm.registers[reg], value, "preserved register {reg}");
        }
        assert_eq!(&self.vm.memory[RAM_START..], expected.as_slice(),
            "full RAM oracle, including guards: dest {dst:?}, source {source}, count {count}");
        assert_eq!((self.vm.registers[5], self.vm.registers[6]),
            ((end % BANK) as u16, (end / BANK) as u16), "advanced canonical pointer");
        // BLOCK/ADDR are write-only MMIO registers and read as zero. The VM
        // maintains their debugger mirrors after each data read; inspect the
        // mirrors and independently check the next word through the device.
        let cursor = source.wrapping_add(count);
        assert_eq!(self.vm.memory[18], cursor,
            "storage advanced exactly once per copied word");
        assert_eq!(self.vm.memory[17], 0, "storage block preserved");
        assert_eq!(self.vm.read_cell(0, 19).unwrap(),
            words.get(cursor as usize).copied().unwrap_or(0), "next unread disk word");
        assert_eq!(self.vm.read_cell(0, 20).unwrap() & 2, 0, "read leaves disk clean");
        self.vm.executed_instructions - before
    }
}

fn main() {
    let root = std::path::PathBuf::from(std::env::args().nth(1)
        .expect("usage: word_read_kernels OUTPUT_DIRECTORY"));
    let binary = fs::read(root.join("word_read_test.bin")).expect("read assembled kernel");
    let disk = root.join("word-read-kernel.disk");
    let mut words: Vec<u16> = (0..BLOCK_WORDS)
        .map(|i| (i.wrapping_mul(3571) ^ 0xfe19) as u16).collect();
    words[..8].copy_from_slice(&[0, 0xffff, 0x8000, 0x7fff, 0x0100, 0x00ff, 0xa55a, 0x5aa5]);
    let bytes: Vec<u8> = words.iter().flat_map(|word| word.to_le_bytes()).collect();
    assert_eq!(bytes.len(), 65_536);
    fs::write(&disk, &bytes).expect("write synthetic disk");
    let mut reports = Vec::new();
    for count in [0u16, 1, 2, 32768] {
        // Ordinary, exactly-ending, and crossing destinations. GP/SB tags are
        // resolved by the assembly itself, including the zero-count case.
        let offsets = [123, 63999, if count == 0 { 0 } else { BANK as u16 - count }];
        let sources: &[u16] = match count {
            0 => &[0, 65535],
            1 => &[0, 1, 32767],
            2 => &[0, 2, 32766],
            _ => &[0],
        };
        for bank in [7u16, 65535, 65534] {
            for offset in offsets {
                for &source in sources {
                    let mut kernel = Kernel::new(&binary, &disk);
                    let steps = kernel.copy((offset, bank), source, count, &words);
                    let next = (kernel.vm.registers[5], kernel.vm.registers[6]);
                    // A returned tag would depend on the caller's later frame
                    // or globals bank. A canonical pointer must not.
                    kernel.vm.registers[28] = 12;
                    kernel.vm.registers[31] = 11;
                    let continuation_steps = kernel.copy(next, 5, 2, &words);
                    reports.push(format!(
                        "{{\"bank\":{bank},\"offset\":{offset},\"source\":{source},\"count\":{count},\"instructions\":{steps},\"continuation_instructions\":{continuation_steps}}}"));
                }
            }
        }
    }
    assert_eq!(fs::read(&disk).unwrap(), bytes, "all copies leave backing disk unchanged");
    let report = format!("{{\"success\":true,\"bank_size\":64000,\"cases\":[{}]}}\n", reports.join(","));
    fs::write(root.join("word-read-kernel-results.json"), report).unwrap();
    println!("Word reader: {} cases passed (u16 disk values, RAM boundaries, tags, returned pointers, guards and saved registers)", reports.len());
}
