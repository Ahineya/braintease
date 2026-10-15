//! Bounded, deterministic discovery of bytecode basic blocks.
use super::{Instr, MAX_BLOCK, MAX_BLOCKS};
use std::collections::HashSet;

pub(super) fn is_plain(opcode: u8) -> bool {
    matches!(opcode, 0x01..=0x11 | 0x1A..=0x20 | 0x42)
}

#[derive(Clone, Copy)]
pub(super) enum Dest {
    Stay(usize),
    Leave,
}

#[derive(Clone, Copy)]
pub(super) enum Edge {
    Next(Dest),
    Branch { taken: Dest, fall: Dest },
}

pub(super) struct GuestBlock {
    pub(super) index: usize,
    pub(super) instrs: Vec<Instr>,
    pub(super) edge: Edge,
}

pub(super) struct Region {
    pub(super) blocks: Vec<GuestBlock>,
}

impl Region {
    pub(super) fn ranges(&self) -> Vec<(usize, usize)> {
        self.blocks
            .iter()
            .map(|block| (block.index, block.instrs.len()))
            .collect()
    }
}

pub(super) fn build_region(
    bank: u16,
    entry: usize,
    code: &[Instr],
    covered: &[bool],
) -> Result<Region, String> {
    if entry >= code.len() {
        return Err(format!("JIT: region entry {entry} is outside code"));
    }
    // Follow branches and the instruction after a call. The callee is a
    // separate native function, entered with a real call.
    let mut leaders = HashSet::new();
    leaders.insert(entry);
    let mut pending = vec![entry];
    let mut cursor = 0;
    while cursor < pending.len() {
        let index = pending[cursor];
        cursor += 1;
        for succ in decode_at(code, bank, index, &HashSet::new(), covered).successors() {
            if succ < code.len()
                && !covered.get(succ).copied().unwrap_or(false)
                && leaders.len() < MAX_BLOCKS
                && leaders.insert(succ)
            {
                pending.push(succ);
            }
        }
    }
    let mut blocks = Vec::new();
    let mut order: Vec<usize> = leaders.iter().copied().collect();
    order.sort_unstable();
    for index in order {
        let mut block = decode_at(code, bank, index, &leaders, covered);
        block.edge = block.edge.resolve(&leaders);
        blocks.push(block);
    }
    Ok(Region { blocks })
}

impl GuestBlock {
    fn successors(&self) -> Vec<usize> {
        self.edge.indices()
    }
}

impl Edge {
    fn indices(&self) -> Vec<usize> {
        match self {
            Edge::Next(dest) => dest.index().into_iter().collect(),
            Edge::Branch { taken, fall } => taken.index().into_iter().chain(fall.index()).collect(),
        }
    }

    fn resolve(self, leaders: &HashSet<usize>) -> Self {
        match self {
            Edge::Next(dest) => Edge::Next(dest.resolve(leaders)),
            Edge::Branch { taken, fall } => Edge::Branch {
                taken: taken.resolve(leaders),
                fall: fall.resolve(leaders),
            },
        }
    }
}

impl Dest {
    fn index(self) -> Option<usize> {
        match self {
            Dest::Stay(index) => Some(index),
            Dest::Leave => None,
        }
    }

    fn resolve(self, leaders: &HashSet<usize>) -> Self {
        match self {
            Dest::Stay(index) if leaders.contains(&index) => Dest::Stay(index),
            _ => Dest::Leave,
        }
    }
}

fn decode_at(
    code: &[Instr],
    bank: u16,
    start: usize,
    stop: &HashSet<usize>,
    covered: &[bool],
) -> GuestBlock {
    let (mut pc, mut pcb) = locate(bank, start);
    let mut index = start;
    let mut instrs = Vec::new();
    loop {
        if index >= code.len() {
            return GuestBlock {
                index: start,
                instrs,
                edge: Edge::Next(Dest::Leave),
            };
        }
        if !instrs.is_empty() && covered.get(index).copied().unwrap_or(false) {
            return GuestBlock {
                index: start,
                instrs,
                edge: Edge::Next(Dest::Leave),
            };
        }
        if !instrs.is_empty() && stop.contains(&index) {
            return GuestBlock {
                index: start,
                instrs,
                edge: Edge::Next(Dest::Stay(index)),
            };
        }
        let inst = code[index];
        instrs.push(inst);
        if let Some(edge) = control_edge(inst, bank, pc, pcb, code.len()) {
            return GuestBlock {
                index: start,
                instrs,
                edge,
            };
        }
        if writes_pc(&inst) {
            return GuestBlock {
                index: start,
                instrs,
                edge: Edge::Next(Dest::Leave),
            };
        }
        if instrs.len() >= MAX_BLOCK {
            let (next_pc, next_pcb) = step_pc(bank, pc, pcb);
            let next = linear(bank, next_pc, next_pcb);
            let dest = if next < code.len() {
                Dest::Stay(next)
            } else {
                Dest::Leave
            };
            return GuestBlock {
                index: start,
                instrs,
                edge: Edge::Next(dest),
            };
        }
        (pc, pcb) = step_pc(bank, pc, pcb);
        index = linear(bank, pc, pcb);
    }
}

fn control_edge(inst: Instr, bank: u16, pc: u16, pcb: u16, len: usize) -> Option<Edge> {
    let inbound = |index: usize| {
        if index < len {
            Dest::Stay(index)
        } else {
            Dest::Leave
        }
    };
    match inst.opcode {
        0x00 | 0x14 | 0x19 | 0x21 => Some(Edge::Next(Dest::Leave)),
        0x12 => None,
        0x13 if inst.word1 == 0 || inst.word1 >= 32 => Some(Edge::Next(if inst.word3 < bank {
            inbound(linear(bank, inst.word3, inst.word2))
        } else {
            Dest::Leave
        })),
        // The callee is a native call. The instruction after JAL stays in this
        // function; the link address does not cross a bank boundary.
        0x13 => Some(Edge::Next(inbound(linear(bank, pc.wrapping_add(1), pcb)))),
        0x15..=0x18 => {
            let taken_pc = (pc as i16).wrapping_add(inst.word3 as i16) as u16;
            let (fall_pc, fall_pcb) = step_pc(bank, pc, pcb);
            Some(Edge::Branch {
                taken: if taken_pc < bank {
                    inbound(linear(bank, taken_pc, pcb))
                } else {
                    Dest::Leave
                },
                fall: inbound(linear(bank, fall_pc, fall_pcb)),
            })
        }
        opcode if is_plain(opcode) => None,
        _ => Some(Edge::Next(Dest::Leave)),
    }
}

pub(super) fn writes_pc(inst: &Instr) -> bool {
    matches!(inst.opcode, 0x01..=0x11 | 0x1A..=0x1F) && matches!(inst.word1, 1 | 2)
}

pub(super) fn locate(bank: u16, index: usize) -> (u16, u16) {
    let bank = usize::from(bank).max(1);
    ((index % bank) as u16, (index / bank) as u16)
}

pub(super) fn linear(bank: u16, pc: u16, pcb: u16) -> usize {
    usize::from(pcb) * usize::from(bank) + usize::from(pc)
}

pub(super) fn step_pc(bank: u16, pc: u16, pcb: u16) -> (u16, u16) {
    let next = u32::from(pc) + 1;
    if next >= u32::from(bank) {
        (0, pcb.wrapping_add(1))
    } else {
        (next as u16, pcb)
    }
}
