//! Bounded control-flow regions; each operation retains its complete excursion.
use super::super::{FastOp, Op, Program, NO_FAST_OP};
use std::collections::{BTreeMap, BTreeSet};

const MAX_NODES: usize = 96;
const MAX_POINTS: usize = 128;
const MAX_UPDATES: usize = 512;
const MAX_OFFSET: isize = 1 << 20;

#[derive(Clone, Debug)]
pub(super) enum Action {
    Arithmetic {
        changes: Vec<(isize, u16)>,
        inverse: Option<u16>,
        next: usize,
    },
    Move {
        next: usize,
    },
    Branch {
        zero: usize,
        nonzero: usize,
    },
}

#[derive(Debug)]
pub(super) struct Node {
    pub pc: usize,
    pub minimum: isize,
    pub maximum: isize,
    pub after: isize,
    pub action: Action,
}

#[derive(Debug)]
pub(super) struct Region {
    pub nodes: BTreeMap<usize, Node>,
}

/// A graph whose pointer has a unique offset at each instruction can prepare
/// its exact read/write cells, preserving unrelated deferred runs and scans.
pub(super) struct Footprint {
    pub offsets: BTreeMap<usize, isize>,
    pub cells: BTreeMap<isize, bool>, // true if any path writes the cell
    pub minimum: isize,
    pub maximum: isize,
}

impl Region {
    pub fn has_cycle(&self) -> bool {
        let mut indegrees: BTreeMap<_, usize> = self.nodes.keys().map(|&pc| (pc, 0)).collect();
        let successors = |node: &Node| match node.action {
            Action::Branch { zero, nonzero } => vec![zero, nonzero],
            Action::Arithmetic { next, .. } | Action::Move { next } => vec![next],
        };
        for node in self.nodes.values() {
            for next in successors(node) {
                if let Some(count) = indegrees.get_mut(&next) {
                    *count += 1;
                }
            }
        }
        let mut work: Vec<_> = indegrees
            .iter()
            .filter_map(|(&pc, &n)| (n == 0).then_some(pc))
            .collect();
        let mut removed = 0;
        while let Some(pc) = work.pop() {
            removed += 1;
            for next in successors(&self.nodes[&pc]) {
                if let Some(count) = indegrees.get_mut(&next) {
                    *count -= 1;
                    if *count == 0 {
                        work.push(next);
                    }
                }
            }
        }
        removed != self.nodes.len()
    }

    pub fn footprint(&self, start: usize) -> Option<Footprint> {
        let mut offsets = BTreeMap::new();
        let mut cells = BTreeMap::new();
        let mut work = vec![(start, 0isize)];
        let (mut minimum, mut maximum) = (0, 0);
        while let Some((pc, offset)) = work.pop() {
            let Some(node) = self.nodes.get(&pc) else {
                continue;
            };
            if let Some(&previous) = offsets.get(&pc) {
                if previous != offset {
                    return None;
                }
                continue;
            }
            offsets.insert(pc, offset);
            minimum = minimum.min(offset.checked_add(node.minimum)?);
            maximum = maximum.max(offset.checked_add(node.maximum)?);
            match &node.action {
                Action::Arithmetic {
                    changes,
                    inverse,
                    next,
                } => {
                    for &(at, _) in changes {
                        cells.insert(offset.checked_add(at)?, true);
                    }
                    if inverse.is_some() {
                        cells.insert(offset, true);
                    }
                    work.push((*next, offset.checked_add(node.after)?));
                }
                Action::Move { next } => work.push((*next, offset.checked_add(node.after)?)),
                Action::Branch { zero, nonzero } => {
                    cells.entry(offset).or_insert(false);
                    work.push((*zero, offset));
                    work.push((*nonzero, offset));
                }
            }
            if cells.len() > MAX_POINTS {
                return None;
            }
        }
        Some(Footprint {
            offsets,
            cells,
            minimum,
            maximum,
        })
    }
    pub fn collect(program: &Program, start: usize, scope_loops: bool) -> Option<Self> {
        let mut region = Self {
            nodes: BTreeMap::new(),
        };
        let mut work = vec![start];
        let mut points = BTreeSet::new();
        let mut updates = 0;
        // Keep a hot inner loop's exit out of the region. Pulling continuation
        // paths into it prepares many cells that its common iterations never
        // touch, and can turn a tiny loop into a large dispatch graph.
        let scope = if scope_loops {
            match program.code.get(start)?.op {
                Op::Open(end) => Some((start, end)),
                _ => program
                    .code
                    .iter()
                    .enumerate()
                    .skip(start)
                    .take(MAX_NODES * 128)
                    .find_map(|(pc, instruction)| match instruction.op {
                        Op::Close(body) if body <= start => Some((body - 1, pc + 1)),
                        _ => None,
                    }),
            }
        } else {
            None
        };
        while let Some(pc) = work.pop() {
            if scope.is_some_and(|(low, high)| pc < low || pc >= high) {
                continue;
            }
            if region.nodes.contains_key(&pc) || region.nodes.len() >= MAX_NODES {
                continue;
            }
            let Some((node, low, high, touched, changed)) = node(program, pc, 0) else {
                continue;
            };
            if low < -MAX_OFFSET || high > MAX_OFFSET || updates + changed.len() > MAX_UPDATES {
                continue;
            }
            let combined: BTreeSet<_> = points
                .iter()
                .copied()
                .chain(touched.iter().copied())
                .collect();
            if combined.len() > MAX_POINTS {
                continue;
            }
            points = combined;
            updates += changed.len();
            match node.action {
                Action::Arithmetic { next, .. } | Action::Move { next } => work.push(next),
                Action::Branch { zero, nonzero } => {
                    work.push(zero);
                    work.push(nonzero);
                }
            }
            region.nodes.insert(pc, node);
        }
        (region.nodes.contains_key(&start) && region.nodes.len() >= 2).then_some(region)
    }
}

type ParsedNode = (Node, isize, isize, Vec<isize>, Vec<isize>);
fn node(program: &Program, pc: usize, offset: isize) -> Option<ParsedNode> {
    let instruction = program.code.get(pc)?;
    let mut after = offset;
    let mut low = offset;
    let mut high = offset;
    let mut touched = Vec::new();
    let mut changed = Vec::new();
    let fast = program.fast_at[pc];
    let action = if fast != NO_FAST_OP {
        let (arithmetic, inverse, next) = match &program.fast[fast as usize] {
            FastOp::Block { arithmetic, next } => (arithmetic, None, *next),
            FastOp::Affine {
                arithmetic,
                inverse,
                next,
            } => (arithmetic, Some(*inverse), *next),
            // Preserve cached scans, deferred fills and guarded routed loops.
            _ => return None,
        };
        after = offset.checked_add(arithmetic.advance)?;
        low = offset.checked_add(arithmetic.minimum)?;
        high = offset.checked_add(arithmetic.maximum)?;
        let mut changes = Vec::new();
        for &(at, delta) in &arithmetic.changes {
            let at = offset.checked_add(at)?;
            changes.push((at, delta));
            touched.push(at);
            changed.push(at);
        }
        if inverse.is_some() {
            touched.push(offset);
            changed.push(offset);
        }
        Action::Arithmetic {
            changes,
            inverse,
            next,
        }
    } else {
        match instruction.op {
            Op::Add(delta) => {
                touched.push(offset);
                changed.push(offset);
                Action::Arithmetic {
                    changes: vec![(offset, delta)],
                    inverse: None,
                    next: pc + 1,
                }
            }
            Op::Move(delta) => {
                after = offset.checked_add(delta)?;
                low = low.min(after);
                high = high.max(after);
                Action::Move { next: pc + 1 }
            }
            Op::Open(next) => {
                touched.push(offset);
                Action::Branch {
                    zero: next,
                    nonzero: pc + 1,
                }
            }
            Op::Close(next) => {
                touched.push(offset);
                Action::Branch {
                    zero: pc + 1,
                    nonzero: next,
                }
            }
            Op::Input | Op::Output | Op::Hook(_) => return None,
        }
    };
    Some((
        Node {
            pc,
            minimum: low,
            maximum: high,
            after,
            action,
        },
        low,
        high,
        touched,
        changed,
    ))
}
