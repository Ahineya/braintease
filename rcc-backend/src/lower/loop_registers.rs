//! Promote private stack cells across a natural inner loop. This is a bounded
//! machine allocation pass: only registers unused by the rest of the body are
//! borrowed, and every entry/exit keeps the existing stack-home contract.
use rcc_codegen::{AsmInst as A, Reg};
use rcc_frontend::{
    ir::{Function, Instruction as I, Value},
    BankTag,
};
use std::collections::{BTreeMap, BTreeSet, HashMap};

const REGS: [Reg; 12] = [
    Reg::T0,
    Reg::T1,
    Reg::T2,
    Reg::T3,
    Reg::T4,
    Reg::T5,
    Reg::T6,
    Reg::T7,
    Reg::S0,
    Reg::S1,
    Reg::S2,
    Reg::S3,
];
fn bit(reg: Reg) -> u32 {
    1 << reg as u32
}
fn return_live() -> u32 {
    [
        Reg::Rv0,
        Reg::Rv1,
        Reg::X0,
        Reg::X1,
        Reg::Ra,
        Reg::Rab,
        Reg::Fp,
        Reg::Sp,
        Reg::Sb,
        Reg::Gp,
    ]
    .into_iter()
    .fold(0, |v, r| v | bit(r))
}

fn effects(inst: &A) -> (u32, u32) {
    use A::*;
    match inst {
        Add(d, a, b)
        | Sub(d, a, b)
        | Mul(d, a, b)
        | Div(d, a, b)
        | Mod(d, a, b)
        | And(d, a, b)
        | Or(d, a, b)
        | Xor(d, a, b)
        | Sll(d, a, b)
        | Srl(d, a, b)
        | Slt(d, a, b)
        | Sltu(d, a, b)
        | Load(d, a, b) => (bit(*a) | bit(*b), bit(*d)),
        Store(a, b, c) => (bit(*a) | bit(*b) | bit(*c), 0),
        AddI(d, a, _)
        | SubI(d, a, _)
        | MulI(d, a, _)
        | DivI(d, a, _)
        | ModI(d, a, _)
        | Move(d, a) => (bit(*a), bit(*d)),
        Li(d, _) | LiLabel(d, _) => (0, bit(*d)),
        Inc(d) | Dec(d) => (bit(*d), bit(*d)),
        Beq(a, b, _) | Bne(a, b, _) | Blt(a, b, _) | Bge(a, b, _) => (bit(*a) | bit(*b), 0),
        Label(_) | Comment(_) => (0, 0),
        _ => (u32::MAX, u32::MAX),
    }
}
fn branch(inst: &A) -> Option<(&str, bool)> {
    match inst {
        A::Beq(a, b, label) => Some((label, a == b)),
        A::Bne(_, _, label) | A::Blt(_, _, label) | A::Bge(_, _, label) => Some((label, false)),
        _ => None,
    }
}
fn terminator(inst: &A) -> bool {
    branch(inst).is_some() || matches!(inst, A::Ret | A::Jalr(Reg::R0, ..) | A::Halt)
}
fn meaningful(inst: &A) -> bool {
    !matches!(inst, A::Label(_) | A::Comment(_))
}
#[derive(Clone)]
struct Block {
    start: usize,
    end: usize,
    last: Option<usize>,
}
struct Cfg {
    blocks: Vec<Block>,
    preds: Vec<Vec<usize>>,
    succs: Vec<Vec<usize>>,
    dom: Vec<BTreeSet<usize>>,
    reachable: BTreeSet<usize>,
}
impl Cfg {
    fn new(code: &[A]) -> Option<Self> {
        if code.is_empty() {
            return None;
        }
        let mut starts = BTreeSet::from([0]);
        for (i, inst) in code.iter().enumerate() {
            if matches!(inst, A::Label(_)) {
                starts.insert(i);
            }
            if terminator(inst) && i + 1 < code.len() {
                starts.insert(i + 1);
            }
        }
        let starts: Vec<_> = starts.into_iter().collect();
        if starts.len() > 1024 {
            return None;
        }
        let blocks: Vec<_> = starts
            .iter()
            .enumerate()
            .map(|(i, &start)| {
                let end = starts.get(i + 1).copied().unwrap_or(code.len());
                Block {
                    start,
                    end,
                    last: (start..end).rev().find(|&j| meaningful(&code[j])),
                }
            })
            .collect();
        let mut labels = HashMap::new();
        for (bi, b) in blocks.iter().enumerate() {
            for inst in &code[b.start..b.end] {
                if let A::Label(label) = inst {
                    if labels.insert(label.as_str(), bi).is_some() {
                        return None;
                    }
                }
            }
        }
        let n = blocks.len();
        let mut preds = vec![vec![]; n];
        let mut succs = vec![vec![]; n];
        for (bi, b) in blocks.iter().enumerate() {
            let mut fall = true;
            if let Some(inst) = b.last.map(|i| &code[i]) {
                if let Some((label, always)) = branch(inst) {
                    succs[bi].push(*labels.get(label)?);
                    fall = !always;
                } else if matches!(inst, A::Ret | A::Jalr(Reg::R0, ..) | A::Halt) {
                    fall = false;
                }
            }
            if fall && bi + 1 < n && !succs[bi].contains(&(bi + 1)) {
                succs[bi].push(bi + 1);
            }
            for &next in &succs[bi] {
                preds[next].push(bi);
            }
        }
        let mut reachable = BTreeSet::new();
        let mut work = vec![0];
        while let Some(b) = work.pop() {
            if reachable.insert(b) {
                work.extend(&succs[b]);
            }
        }
        let mut dom = vec![reachable.clone(); n];
        dom[0] = BTreeSet::from([0]);
        loop {
            let mut changed = false;
            for &b in reachable.iter().filter(|&&b| b != 0) {
                let mut d = reachable.clone();
                for &p in &preds[b] {
                    if reachable.contains(&p) {
                        d.retain(|x| dom[p].contains(x));
                    }
                }
                d.insert(b);
                if dom[b] != d {
                    dom[b] = d;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        Some(Self {
            blocks,
            preds,
            succs,
            dom,
            reachable,
        })
    }
    fn liveness(&self, code: &[A]) -> (Vec<u32>, Vec<u32>, Vec<u32>) {
        let n = self.blocks.len();
        let mut input = vec![0; n];
        let mut output = vec![0; n];
        loop {
            let mut changed = false;
            for b in (0..n).rev() {
                let mut live = if self.succs[b].is_empty() {
                    return_live()
                } else {
                    0
                };
                for &s in &self.succs[b] {
                    live |= input[s];
                }
                let out = live;
                for inst in code[self.blocks[b].start..self.blocks[b].end].iter().rev() {
                    let (u, d) = effects(inst);
                    live = (live & !d) | u;
                }
                if input[b] != live || output[b] != out {
                    input[b] = live;
                    output[b] = out;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        let mut after = vec![0; code.len()];
        for (b, block) in self.blocks.iter().enumerate() {
            let mut live = output[b];
            for i in (block.start..block.end).rev() {
                after[i] = live;
                let (u, d) = effects(&code[i]);
                live = (live & !d) | u;
            }
        }
        (input, output, after)
    }
    fn inner_loops(&self) -> Vec<(usize, BTreeSet<usize>)> {
        let mut loops = BTreeMap::<usize, BTreeSet<usize>>::new();
        for &tail in &self.reachable {
            for &head in &self.succs[tail] {
                if self.dom[tail].contains(&head) {
                    let members = loops.entry(head).or_insert_with(|| BTreeSet::from([head]));
                    let mut work = vec![tail];
                    while let Some(b) = work.pop() {
                        if members.insert(b) {
                            work.extend(
                                self.preds[b]
                                    .iter()
                                    .copied()
                                    .filter(|p| self.reachable.contains(p)),
                            );
                        }
                    }
                }
            }
        }
        let all: Vec<_> = loops.into_iter().collect();
        all.iter()
            .filter(|(_, members)| {
                !all.iter()
                    .any(|(_, other)| other.len() < members.len() && other.is_subset(members))
            })
            .cloned()
            .collect()
    }
}

type Addresses = [Option<i16>; 32];
fn transfer(inst: &A, known: &mut Addresses) {
    let next = match inst {
        A::Add(d, a, Reg::R0) | A::Add(d, Reg::R0, a) | A::Move(d, a) => {
            Some((*d, known[*a as usize]))
        }
        A::AddI(d, a, v) => Some((*d, known[*a as usize].map(|x| x.wrapping_add(*v)))),
        A::SubI(d, a, v) => Some((*d, known[*a as usize].map(|x| x.wrapping_sub(*v)))),
        _ => None,
    };
    let (_, defs) = effects(inst);
    for (r, value) in known.iter_mut().enumerate() {
        if defs & (1 << r) != 0 {
            *value = None;
        }
    }
    if let Some((d, v)) = next {
        if d != Reg::R0 {
            known[d as usize] = v;
        }
    }
    known[Reg::Fp as usize] = Some(0);
}
fn accesses(code: &[A], cfg: &Cfg) -> Vec<Option<i16>> {
    let mut inputs = vec![[None; 32]; cfg.blocks.len()];
    let mut outputs = inputs.clone();
    for state in &mut inputs {
        state[Reg::Fp as usize] = Some(0);
    }
    for _ in 0..cfg.blocks.len() + 1 {
        let mut changed = false;
        for &b in &cfg.reachable {
            let mut state = [None; 32];
            state[Reg::Fp as usize] = Some(0);
            let predecessors: Vec<_> = cfg.preds[b]
                .iter()
                .copied()
                .filter(|p| cfg.reachable.contains(p))
                .collect();
            if let Some((&first, rest)) = predecessors.split_first() {
                state = outputs[first];
                for &p in rest {
                    for r in 0..32 {
                        if state[r] != outputs[p][r] {
                            state[r] = None;
                        }
                    }
                }
                state[Reg::Fp as usize] = Some(0);
            }
            inputs[b] = state;
            for inst in &code[cfg.blocks[b].start..cfg.blocks[b].end] {
                transfer(inst, &mut state);
            }
            if state != outputs[b] {
                outputs[b] = state;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    let mut result = vec![None; code.len()];
    for (b, block) in cfg.blocks.iter().enumerate() {
        let mut state = inputs[b];
        for i in block.start..block.end {
            if let A::Load(_, Reg::Sb, addr) | A::Store(_, Reg::Sb, addr) = code[i] {
                result[i] = state[addr as usize];
            }
            transfer(&code[i], &mut state);
        }
    }
    result
}

fn alloca(value: &Value) -> Option<u32> {
    match value {
        Value::Temp(id) => Some(*id),
        Value::FatPtr(p) if p.bank == BankTag::Stack => match &*p.addr {
            Value::Temp(id) => Some(*id),
            _ => None,
        },
        _ => None,
    }
}
fn private_cells(
    function: &Function,
    offsets: &HashMap<u32, i16>,
    locals: i16,
    top: i16,
) -> BTreeSet<i16> {
    let mut candidates: BTreeSet<_> = function
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter_map(|i| match i {
            I::Alloca {
                result,
                alloc_type,
                count,
                ..
            } if alloc_type.is_integer()
                && alloc_type.size_in_words() == Some(1)
                && matches!(count, None | Some(Value::Constant(1))) =>
            {
                Some(*result)
            }
            _ => None,
        })
        .collect();
    for inst in function.blocks.iter().flat_map(|b| &b.instructions) {
        let mut uses = crate::optimize::used_temps(inst);
        match inst {
            I::Load {
                ptr,
                volatile: false,
                ..
            } => {
                if let Some(id) = alloca(ptr) {
                    uses.remove(&id);
                }
            }
            I::Store {
                ptr,
                value,
                volatile: false,
                ..
            } => {
                if let Some(id) = alloca(ptr) {
                    uses.remove(&id);
                    crate::optimize::value_temps(value, &mut uses);
                }
            }
            _ => {}
        }
        for id in uses {
            candidates.remove(&id);
        }
    }
    candidates
        .into_iter()
        .filter_map(|id| offsets.get(&id).copied())
        .chain(locals..top)
        .collect()
}
fn safe_ir(function: &Function) -> bool {
    !function
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .any(|i| match i {
            I::InlineAsm {
                assembly, clobbers, ..
            } => !clobbers.is_empty() || !crate::optimize::safe_asm(assembly, clobbers),
            _ => false,
        })
}

/// At most one inner loop per function is promoted. Keeping the allocation
/// local and bounded avoids changing the allocator's contract on other edges.
pub(super) fn promote(
    code: &mut Vec<A>,
    function: &Function,
    offsets: &HashMap<u32, i16>,
    locals: i16,
    top: i16,
) {
    if !safe_ir(function) {
        return;
    }
    let Some(begin) = code.iter().position(|i| {
        matches!(i, A::Comment(s) if s.starts_with("Reserve ") && s.ends_with(" spill slots above locals"))
    }).map(|i| i + 2) else {
        return;
    };
    let Some(end) = code
        .iter()
        .position(|i| matches!(i,A::Comment(s)if s=="=== Function Epilogue ==="))
    else {
        return;
    };
    let body = &code[begin..end];
    if body
        .iter()
        .any(|i| matches!(i, A::Raw(_) | A::Push(_) | A::Pop(_)))
    {
        return;
    }
    if body.iter().any(|i| {
        effects(i).1 & (bit(Reg::Fp) | bit(Reg::Sb)) != 0
            && !matches!(i, A::Call(_) | A::Jal(..) | A::Jalr(..))
    }) {
        return;
    }
    let used = body
        .iter()
        .filter(|i| !matches!(i, A::Call(_) | A::Jal(..) | A::Jalr(..)))
        .fold(0, |used, i| {
            let (u, d) = effects(i);
            used | u | d
        });
    let free: Vec<_> = REGS.into_iter().filter(|&r| used & bit(r) == 0).collect();
    if free.is_empty() {
        return;
    }
    let Some(cfg) = Cfg::new(body) else {
        return;
    };
    let slots = accesses(body, &cfg);
    let private = private_cells(function, offsets, locals, top);
    let (live_in, live_out, after) = cfg.liveness(body);
    let mut choices = vec![];
    for (head, members) in cfg.inner_loops() {
        let outside: Vec<_> = cfg.preds[head]
            .iter()
            .copied()
            .filter(|p| cfg.reachable.contains(p) && !members.contains(p))
            .collect();
        if outside.len() != 1 {
            continue;
        }
        let pre = outside[0];
        if cfg.succs[pre] != [head] {
            continue;
        }
        if members.iter().any(|&b| {
            b != head
                && cfg.preds[b]
                    .iter()
                    .any(|p| cfg.reachable.contains(p) && !members.contains(p))
        }) {
            continue;
        }
        let exits: BTreeSet<_> = members
            .iter()
            .flat_map(|&b| &cfg.succs[b])
            .filter(|b| !members.contains(b))
            .copied()
            .collect();
        if exits.is_empty()
            || exits.iter().any(|&e| {
                cfg.preds[e]
                    .iter()
                    .any(|p| cfg.reachable.contains(p) && !members.contains(p))
                    || live_in[e] & bit(Reg::Sc) != 0
            })
        {
            continue;
        }
        if members.iter().any(|&b| {
            body[cfg.blocks[b].start..cfg.blocks[b].end]
                .iter()
                .any(|i| {
                    matches!(
                        i,
                        A::Call(_) | A::Jal(..) | A::Jalr(..) | A::Ret | A::Halt | A::Brk
                    )
                })
        }) {
            continue;
        }
        // Every direct stack-bank access must be classified; otherwise an
        // unrecognized reload could observe a stale promoted home.
        if members.iter().any(|&b| {
            (cfg.blocks[b].start..cfg.blocks[b].end).any(|i| {
                matches!(body[i], A::Load(_, Reg::Sb, _) | A::Store(_, Reg::Sb, _))
                    && slots[i].is_none()
            })
        }) {
            continue;
        }
        let insert = cfg.blocks[pre]
            .last
            .filter(|&i| terminator(&body[i]))
            .unwrap_or(cfg.blocks[pre].end);
        let live = if insert < cfg.blocks[pre].end {
            let (u, d) = effects(&body[insert]);
            (after[insert] & !d) | u
        } else {
            live_out[pre]
        };
        if live & bit(Reg::Sc) != 0 {
            continue;
        }
        let mut counts = BTreeMap::<i16, (usize, bool)>::new();
        for &b in &members {
            for i in cfg.blocks[b].start..cfg.blocks[b].end {
                if let Some(slot) = slots[i].filter(|s| private.contains(s)) {
                    let item = counts.entry(slot).or_default();
                    item.0 += 1;
                    item.1 |= matches!(body[i], A::Store(..));
                }
            }
        }
        let mut hot: Vec<_> = counts.into_iter().collect();
        hot.sort_by_key(|(slot, (count, _))| (std::cmp::Reverse(*count), *slot));
        hot.truncate(free.len());
        if hot.is_empty() {
            continue;
        }
        let score: usize = hot.iter().map(|(_, (count, _))| count).sum();
        choices.push((score, head, members, exits, insert, hot));
    }
    choices.sort_by_key(|(score, head, ..)| (std::cmp::Reverse(*score), *head));
    let Some((_, _, members, exits, insert, hot)) = choices.into_iter().next() else {
        return;
    };
    let promoted: BTreeMap<_, _> = hot
        .iter()
        .zip(&free)
        .map(|((slot, (_, dirty)), reg)| (*slot, (*reg, *dirty)))
        .collect();
    let mut before = BTreeMap::<usize, Vec<A>>::new();
    let entry = before.entry(insert).or_default();
    entry.push(A::Comment(
        "Cache private loop cells in unused registers".into(),
    ));
    for (&slot, &(reg, _)) in &promoted {
        entry.extend([
            A::AddI(Reg::Sc, Reg::Fp, slot),
            A::Load(reg, Reg::Sb, Reg::Sc),
        ]);
    }
    for exit in exits {
        let b = &cfg.blocks[exit];
        let at = (b.start..b.end)
            .find(|&i| meaningful(&body[i]))
            .unwrap_or(b.end);
        let out = before.entry(at).or_default();
        for (&slot, &(reg, dirty)) in &promoted {
            if dirty {
                out.extend([
                    A::AddI(Reg::Sc, Reg::Fp, slot),
                    A::Store(reg, Reg::Sb, Reg::Sc),
                ]);
            }
        }
    }
    let mut replacement = BTreeMap::new();
    for b in members {
        for i in cfg.blocks[b].start..cfg.blocks[b].end {
            if let Some((reg, _)) = slots[i].and_then(|s| promoted.get(&s)) {
                match body[i] {
                    A::Load(dest, ..) => {
                        replacement.insert(i, A::Move(dest, *reg));
                    }
                    A::Store(src, ..) => {
                        replacement.insert(i, A::Move(*reg, src));
                    }
                    _ => {}
                }
            }
        }
    }
    let mut changed = vec![];
    for (i, inst) in body.iter().enumerate() {
        if let Some(extra) = before.remove(&i) {
            changed.extend(extra);
        }
        changed.push(replacement.remove(&i).unwrap_or_else(|| inst.clone()));
    }
    if let Some(extra) = before.remove(&body.len()) {
        changed.extend(extra);
    }
    cleanup(&mut changed);
    code.splice(begin..end, changed);
}

fn pure_write(inst: &A) -> Option<Reg> {
    match inst {
        A::Add(d, ..)
        | A::Sub(d, ..)
        | A::Mul(d, ..)
        | A::And(d, ..)
        | A::Or(d, ..)
        | A::Xor(d, ..)
        | A::Sll(d, ..)
        | A::Srl(d, ..)
        | A::Slt(d, ..)
        | A::Sltu(d, ..)
        | A::AddI(d, ..)
        | A::SubI(d, ..)
        | A::MulI(d, ..)
        | A::Move(d, ..)
        | A::Li(d, ..)
        | A::LiLabel(d, ..) => Some(*d),
        _ => None,
    }
}
fn rewrite_reads(inst: &mut A, aliases: &[Reg; 32]) {
    let r = |x: &mut Reg| *x = aliases[*x as usize];
    match inst {
        A::Add(_, a, b)
        | A::Sub(_, a, b)
        | A::Mul(_, a, b)
        | A::Div(_, a, b)
        | A::Mod(_, a, b)
        | A::And(_, a, b)
        | A::Or(_, a, b)
        | A::Xor(_, a, b)
        | A::Sll(_, a, b)
        | A::Srl(_, a, b)
        | A::Slt(_, a, b)
        | A::Sltu(_, a, b)
        | A::Load(_, a, b)
        | A::Beq(a, b, _)
        | A::Bne(a, b, _)
        | A::Blt(a, b, _)
        | A::Bge(a, b, _) => {
            r(a);
            r(b);
        }
        A::Store(a, b, c) => {
            r(a);
            r(b);
            r(c);
        }
        A::AddI(_, a, _)
        | A::SubI(_, a, _)
        | A::MulI(_, a, _)
        | A::DivI(_, a, _)
        | A::ModI(_, a, _)
        | A::Move(_, a) => r(a),
        _ => {}
    }
}
fn set_dest(inst: &mut A, target: Reg) {
    match inst {
        A::Add(d, ..)
        | A::Sub(d, ..)
        | A::Mul(d, ..)
        | A::And(d, ..)
        | A::Or(d, ..)
        | A::Xor(d, ..)
        | A::Sll(d, ..)
        | A::Srl(d, ..)
        | A::Slt(d, ..)
        | A::Sltu(d, ..)
        | A::AddI(d, ..)
        | A::SubI(d, ..)
        | A::MulI(d, ..)
        | A::Move(d, ..)
        | A::Li(d, ..)
        | A::LiLabel(d, ..) => *d = target,
        _ => {}
    }
}
fn cleanup(code: &mut Vec<A>) {
    for _ in 0..4 {
        let Some(cfg) = Cfg::new(code) else {
            return;
        };
        for block in &cfg.blocks {
            let mut aliases = std::array::from_fn(|i| Reg::from_u8(i as u8).unwrap());
            let mut constants = [None; 32];
            constants[Reg::R0 as usize] = Some(0i16);
            for i in block.start..block.end {
                rewrite_reads(&mut code[i], &aliases);
                match code[i].clone() {
                    A::Add(d, a, b) => {
                        if let Some(v) = constants[b as usize] {
                            code[i] = A::AddI(d, a, v);
                        } else if let Some(v) = constants[a as usize] {
                            code[i] = A::AddI(d, b, v);
                        }
                    }
                    A::Sub(d, a, b) => {
                        if let Some(v) = constants[b as usize] {
                            code[i] = A::AddI(d, a, v.wrapping_neg());
                        }
                    }
                    _ => {}
                }
                let new_alias = if let A::Move(d, a) = code[i] {
                    Some((d, aliases[a as usize]))
                } else {
                    None
                };
                let new_constant = match code[i] {
                    A::Li(d, v) => Some((d, Some(v))),
                    A::Move(d, a) => Some((d, constants[a as usize])),
                    _ => None,
                };
                let (_, defs) = effects(&code[i]);
                for r in 0..32 {
                    if defs & bit(aliases[r]) != 0 || defs & (1 << r) != 0 {
                        aliases[r] = Reg::from_u8(r as u8).unwrap();
                    }
                    if defs & (1 << r) != 0 {
                        constants[r] = None;
                    }
                }
                if let Some((d, a)) = new_alias {
                    if d != Reg::R0 {
                        aliases[d as usize] = a;
                    }
                }
                if let Some((d, v)) = new_constant {
                    if d != Reg::R0 {
                        constants[d as usize] = v;
                    }
                }
            }
        }
        let Some(cfg) = Cfg::new(code) else {
            return;
        };
        let (_, _, after) = cfg.liveness(code);
        for block in &cfg.blocks {
            let real: Vec<_> = (block.start..block.end)
                .filter(|&i| meaningful(&code[i]))
                .collect();
            for pair in real.windows(2) {
                let (a, b) = (pair[0], pair[1]);
                if let (Some(source), A::Move(target, input)) =
                    (pure_write(&code[a]), code[b].clone())
                {
                    if source != Reg::R0
                        && source == input
                        && after[b] & bit(source) == 0
                        && target != Reg::R0
                    {
                        set_dest(&mut code[a], target);
                        code[b] = A::Comment("Coalesced loop register copy".into());
                    }
                }
            }
        }
        let Some(cfg) = Cfg::new(code) else {
            return;
        };
        let (_, _, after) = cfg.liveness(code);
        let mut i = 0;
        code.retain(|inst| {
            let keep = pure_write(inst).is_none_or(|d| d != Reg::R0 && after[i] & bit(d) != 0);
            i += 1;
            keep
        });
    }
}

#[cfg(test)]
mod tests;
