//! Small instruction interpreter for compiler regressions. Checks the emitted
//! code's behavior, including spills and branches, without changing either VM.
use std::collections::HashMap;
use rcc_codegen::{AsmInst, Reg};

pub struct Machine {
    pub regs: [u16; 32],
    pub memory: HashMap<(u16, u16), u16>,
    pub executed_instructions: usize,
    bank_size: u16,
}

impl Machine {
    pub fn new(bank_size: u16) -> Self {
        let mut regs = [0; 32];
        regs[Reg::Gp as usize] = 1;
        regs[Reg::Sb as usize] = 2;
        Self { regs, memory: HashMap::new(), executed_instructions: 0, bank_size }
    }

    pub fn run(&mut self, insts: &[AsmInst]) {
        let labels: HashMap<_, _> = insts.iter().enumerate().filter_map(|(i, inst)| {
            if let AsmInst::Label(label) = inst { Some((label.as_str(), i)) } else { None }
        }).collect();
        let mut pc = 0;
        let mut steps = 0;
        while pc < insts.len() {
            steps += 1;
            assert!(steps < 1_000_000, "Generated code did not terminate");
            if !matches!(insts[pc], AsmInst::Label(_) | AsmInst::Comment(_)) {
                self.executed_instructions += 1;
            }
            let r = &self.regs;
            let mut write = None;
            match &insts[pc] {
                AsmInst::Li(d, v) => write = Some((*d, *v as u16)),
                AsmInst::Move(d, a) => write = Some((*d, r[*a as usize])),
                AsmInst::Add(d, a, b) => write = Some((*d, r[*a as usize].wrapping_add(r[*b as usize]))),
                AsmInst::Sub(d, a, b) => write = Some((*d, r[*a as usize].wrapping_sub(r[*b as usize]))),
                AsmInst::Mul(d, a, b) => write = Some((*d, r[*a as usize].wrapping_mul(r[*b as usize]))),
                AsmInst::Div(d, a, b) => write = Some((*d, r[*a as usize] / r[*b as usize])),
                AsmInst::Mod(d, a, b) => write = Some((*d, r[*a as usize] % r[*b as usize])),
                AsmInst::AddI(d, a, b) => write = Some((*d, r[*a as usize].wrapping_add(*b as u16))),
                AsmInst::SubI(d, a, b) => write = Some((*d, r[*a as usize].wrapping_sub(*b as u16))),
                AsmInst::MulI(d, a, b) => write = Some((*d, r[*a as usize].wrapping_mul(*b as u16))),
                AsmInst::DivI(d, a, b) => write = Some((*d, r[*a as usize] / *b as u16)),
                AsmInst::ModI(d, a, b) => write = Some((*d, r[*a as usize] % *b as u16)),
                AsmInst::Sll(d, a, b) => write = Some((*d, r[*a as usize] << (r[*b as usize] & 15))),
                AsmInst::Srl(d, a, b) => write = Some((*d, r[*a as usize] >> (r[*b as usize] & 15))),
                AsmInst::And(d, a, b) => write = Some((*d, r[*a as usize] & r[*b as usize])),
                AsmInst::Or(d, a, b) => write = Some((*d, r[*a as usize] | r[*b as usize])),
                AsmInst::Xor(d, a, b) => write = Some((*d, r[*a as usize] ^ r[*b as usize])),
                AsmInst::Slt(d, a, b) => write = Some((*d, ((r[*a as usize] as i16) < (r[*b as usize] as i16)) as u16)),
                AsmInst::Sltu(d, a, b) => write = Some((*d, (r[*a as usize] < r[*b as usize]) as u16)),
                AsmInst::Load(d, bank, addr) => {
                    let key = (r[*bank as usize], r[*addr as usize]);
                    assert!(key.1 < self.bank_size, "Out-of-bank load: {key:?}");
                    write = Some((*d, *self.memory.get(&key).unwrap_or(&0)));
                }
                AsmInst::Store(src, bank, addr) => {
                    let key = (r[*bank as usize], r[*addr as usize]);
                    assert!(key.1 < self.bank_size, "Out-of-bank store: {key:?}");
                    self.memory.insert(key, r[*src as usize]);
                }
                AsmInst::Beq(a, b, label) => if r[*a as usize] == r[*b as usize] { pc = labels[label.as_str()]; },
                AsmInst::Bne(a, b, label) => if r[*a as usize] != r[*b as usize] { pc = labels[label.as_str()]; },
                AsmInst::Blt(a, b, label) => if (r[*a as usize] as i16) < (r[*b as usize] as i16) { pc = labels[label.as_str()]; },
                AsmInst::Bge(a, b, label) => if (r[*a as usize] as i16) >= (r[*b as usize] as i16) { pc = labels[label.as_str()]; },
                AsmInst::Label(_) | AsmInst::Comment(_) => {},
                inst => panic!("Unsupported test instruction: {inst:?}"),
            }
            if let Some((d, value)) = write { if d != Reg::R0 { self.regs[d as usize] = value; } }
            pc += 1;
        }
    }
}
