//! Lexical loop regions plus forward branch scopes. Graphs that cannot be
//! represented with these nested regions use a per-function br_table fallback.
use crate::emit::Cg;
use rcc_ir::ir::{Instruction as I, IrType};
use std::{
    borrow::Cow,
    collections::{BTreeMap, BTreeSet},
};
use wasm_encoder::{BlockType, Instruction as W};

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Label {
    Forward(usize),
    Loop(usize),
    Dispatch,
    Other,
}

fn targets(i: &I) -> Vec<u32> {
    match i {
        I::Branch(to) => vec![*to],
        I::BranchCond {
            true_label,
            false_label,
            ..
        } => vec![*true_label, *false_label],
        _ => Vec::new(),
    }
}

pub(crate) fn emit(cg: &mut Cg<'_>) -> Result<bool, String> {
    let blocks = &cg.function.blocks;
    if blocks.is_empty() {
        return Err("function has no body".into());
    }
    let mut indices = BTreeMap::new();
    for (i, b) in blocks.iter().enumerate() {
        if indices.insert(b.id, i).is_some() {
            return Err(format!("duplicate block {}", b.id));
        }
        if !b.has_terminator() {
            return Err(format!("block {} has no terminator", b.id));
        }
        for inst in &b.instructions[..b.instructions.len() - 1] {
            if matches!(inst, I::Return(_) | I::Branch(_) | I::BranchCond { .. }) {
                return Err(format!(
                    "block {} has instructions after a terminator",
                    b.id
                ));
            }
        }
    }
    let mut edges = Vec::new();
    for (i, b) in blocks.iter().enumerate() {
        for to in targets(b.instructions.last().unwrap()) {
            edges.push((
                i,
                *indices
                    .get(&to)
                    .ok_or_else(|| format!("unknown block {to}"))?,
            ));
        }
    }
    for (index, b) in blocks.iter().enumerate() {
        let predecessors: BTreeSet<_> = edges
            .iter()
            .filter(|(_, to)| *to == index)
            .map(|(from, _)| blocks[*from].id)
            .collect();
        let mut body_started = false;
        for inst in &b.instructions {
            match inst {
                I::Phi {
                    result, incoming, ..
                } => {
                    let labels: BTreeSet<_> = incoming.iter().map(|(_, label)| *label).collect();
                    if index == 0
                        || body_started
                        || labels != predecessors
                        || labels.len() != incoming.len()
                    {
                        return Err(format!(
                            "invalid incoming edges or placement for phi %{result} in block {}",
                            b.id
                        ));
                    }
                }
                I::Comment(_) | I::DebugLoc { .. } => {}
                _ => body_started = true,
            }
        }
    }
    crate::verify::ssa(cg.function, &edges, &indices)?;
    cg.block_indices = indices;
    let mut loops = BTreeMap::<usize, usize>::new();
    for &(from, to) in &edges {
        if to <= from {
            loops
                .entry(to)
                .and_modify(|end| *end = (*end).max(from))
                .or_insert(from);
        }
    }
    let structured = loops.iter().all(|(&start, &end)| {
        // Loop regions must be nested/disjoint, and entered only at the header.
        loops
            .iter()
            .filter(|(s, _)| **s > start && **s <= end)
            .all(|(_, e)| *e <= end)
            && edges
                .iter()
                .all(|&(from, to)| !(to > start && to <= end && (from < start || from > end)))
    });
    if structured {
        cg.region(0, blocks.len() - 1, &loops, None)?;
    } else {
        cg.dispatcher()?;
    }
    Ok(structured)
}

impl Cg<'_> {
    fn open(&mut self, label: Label, is_loop: bool) {
        self.ins(if is_loop {
            W::Loop(BlockType::Empty)
        } else {
            W::Block(BlockType::Empty)
        });
        self.labels.push(label);
    }
    fn close(&mut self) {
        self.ins(W::End);
        self.labels.pop();
    }
    fn depth(&self, label: Label) -> Result<u32, String> {
        self.labels
            .iter()
            .rev()
            .position(|l| *l == label)
            .map(|i| i as u32)
            .ok_or("Wasm branch escaped its CFG region".into())
    }
    fn region(
        &mut self,
        start: usize,
        end: usize,
        loops: &BTreeMap<usize, usize>,
        active: Option<usize>,
    ) -> Result<(), String> {
        let mut units = Vec::new();
        let mut i = start;
        while i <= end {
            let loop_end = loops.get(&i).copied().filter(|_| Some(i) != active);
            units.push((i, loop_end));
            i = loop_end.unwrap_or(i) + 1;
        }
        // The innermost forward scope ends at the earliest following block.
        for &(index, _) in units.iter().skip(1).rev() {
            self.open(Label::Forward(index), false);
        }
        for (position, &(index, loop_end)) in units.iter().enumerate() {
            if position > 0 {
                self.close();
            }
            if let Some(end) = loop_end {
                self.open(Label::Loop(index), true);
                self.region(index, end, loops, Some(index))?;
                self.close();
            } else {
                self.block(index)?;
            }
        }
        Ok(())
    }
    fn dispatcher(&mut self) -> Result<(), String> {
        self.dispatch = true;
        self.ins(W::I32Const(0));
        self.ins(W::LocalSet(self.pc));
        self.open(Label::Dispatch, true);
        let count = self.function.blocks.len();
        self.open(Label::Other, false); // invalid PC lands on unreachable
        for _ in (0..count).rev() {
            self.open(Label::Other, false);
        }
        self.ins(W::LocalGet(self.pc));
        self.ins(W::BrTable(
            Cow::Owned((0..count as u32).collect()),
            count as u32,
        ));
        for index in 0..count {
            self.close();
            self.block(index)?;
        }
        self.close();
        self.ins(W::Unreachable);
        self.close();
        Ok(())
    }
    fn block(&mut self, index: usize) -> Result<(), String> {
        let block = &self.function.blocks[index];
        for inst in &block.instructions {
            let result = match inst {
                I::Return(value) => {
                    match (value, &self.function.return_type) {
                        (None, IrType::Void) => self.ins(W::I64Const(0)),
                        (Some(v), ty) if *ty != IrType::Void => {
                            self.value(v)?;
                            self.mask(crate::emit::width(ty)?);
                        }
                        _ => return Err("return does not match function result type".into()),
                    }
                    self.ins(W::LocalGet(self.mark));
                    self.ins(W::GlobalSet(crate::memory::STACK));
                    self.ins(W::Return);
                    Ok(())
                }
                I::Branch(to) => self.edge(index, *to),
                I::BranchCond {
                    condition,
                    true_label,
                    false_label,
                } => {
                    self.value(condition)?;
                    self.ins(W::I64Eqz);
                    self.ins(W::I32Eqz);
                    self.ins(W::If(BlockType::Empty));
                    self.labels.push(Label::Other);
                    self.edge(index, *true_label)?;
                    self.ins(W::Else);
                    self.edge(index, *false_label)?;
                    self.close();
                    Ok(())
                }
                _ => self.instruction(inst),
            };
            result.map_err(|e| format!("block {}: {e}", block.id))?;
        }
        Ok(())
    }
    fn edge(&mut self, from: usize, to: u32) -> Result<(), String> {
        let index = *self
            .block_indices
            .get(&to)
            .ok_or_else(|| format!("unknown block {to}"))?;
        // Evaluate all incoming values first; a loop backedge may swap phi
        // locals. The operand stack is the parallel-copy scratch space.
        let phis: Vec<_> = self.function.blocks[index]
            .instructions
            .iter()
            .filter_map(|i| {
                if let I::Phi {
                    result, incoming, ..
                } = i
                {
                    Some((
                        *result,
                        incoming
                            .iter()
                            .find(|(_, label)| *label == self.function.blocks[from].id)
                            .map(|(v, _)| v),
                    ))
                } else {
                    None
                }
            })
            .collect();
        for (result, value) in &phis {
            self.value(value.ok_or_else(|| format!("phi %{result} has no incoming edge"))?)?;
        }
        for (result, _) in phis.iter().rev() {
            self.define(*result)?;
        }
        if self.dispatch {
            self.ins(W::I32Const(index as i32));
            self.ins(W::LocalSet(self.pc));
            self.ins(W::Br(self.depth(Label::Dispatch)?));
        } else {
            let label = if index <= from {
                Label::Loop(index)
            } else {
                Label::Forward(index)
            };
            self.ins(W::Br(self.depth(label)?));
        }
        Ok(())
    }
}
