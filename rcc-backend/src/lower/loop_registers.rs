//! Promote private stack cells across a natural inner loop. This is a bounded
//! machine allocation pass: registers unused and dead through the selected loop
//! are borrowed, and every entry/exit preserves observable stack-home values.
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
const ARGUMENTS: [Reg; 4] = [Reg::A0, Reg::A1, Reg::A2, Reg::A3];
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
    if let Some(header) = promote_once(code, function, offsets, locals, top, None) {
        // Copy cleanup can release registers occupied by duplicate cached
        // values. Recompute every proof before filling those registers, and
        // keep this second, bounded round on the same inner loop.
        promote_once(code, function, offsets, locals, top, Some(&header));
    }
    // Equality branches are useful even when allocation found no eligible
    // stack cells. Keep this independent cleanup inside the original body so
    // frame construction and the prologue/epilogue markers remain intact.
    if safe_ir(function) {
        if let Some(range) = body_range(code) {
            let mut body = code[range.clone()].to_vec();
            if let Some(cfg) = Cfg::new(&body) {
                if fuse_comparison_branches(&mut body, &cfg) {
                    cleanup_private(&mut body, &BTreeSet::new());
                    code.splice(range, body);
                }
            }
        }
    }
}

fn body_range(code: &[A]) -> Option<std::ops::Range<usize>> {
    let begin = code.iter().position(|i| {
        matches!(i, A::Comment(s) if s.starts_with("Reserve ") && s.ends_with(" spill slots above locals"))
    })? + 2;
    let end = code
        .iter()
        .position(|i| matches!(i,A::Comment(s)if s=="=== Function Epilogue ==="))?;
    (begin <= end).then_some(begin..end)
}

fn promote_once(
    code: &mut Vec<A>,
    function: &Function,
    offsets: &HashMap<u32, i16>,
    locals: i16,
    top: i16,
    only_header: Option<&str>,
) -> Option<String> {
    if !safe_ir(function) {
        return None;
    }
    let range = body_range(code)?;
    let (begin, end) = (range.start, range.end);
    let body = &code[begin..end];
    if body
        .iter()
        .any(|i| matches!(i, A::Raw(_) | A::Push(_) | A::Pop(_)))
    {
        return None;
    }
    if body.iter().any(|i| {
        effects(i).1 & (bit(Reg::Fp) | bit(Reg::Sb)) != 0
            && !matches!(i, A::Call(_) | A::Jal(..) | A::Jalr(..))
    }) {
        return None;
    }
    let used = body
        .iter()
        .filter(|i| !matches!(i, A::Call(_) | A::Jal(..) | A::Jalr(..)))
        .fold(0, |used, i| {
            let (u, d) = effects(i);
            used | u | d
        });
    let Some(cfg) = Cfg::new(body) else {
        return None;
    };
    let slots = accesses(body, &cfg);
    let private = private_cells(function, offsets, locals, top);
    let (live_in, live_out, after) = cfg.liveness(body);
    let mut choices = vec![];
    for (head, members) in cfg.inner_loops() {
        let label = body[cfg.blocks[head].start..cfg.blocks[head].end]
            .iter()
            .find_map(|inst| match inst {
                A::Label(label) => Some(label),
                _ => None,
            });
        if label.is_none()
            || only_header.is_some_and(|wanted| label.map(String::as_str) != Some(wanted))
        {
            continue;
        }
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
        // A register used by setup or a different loop can still be borrowed
        // here. Reject every explicit use/write in this region and every value
        // live across its boundaries, including zero-trip and early-exit paths.
        let region_used = members.iter().fold(live, |used, &b| {
            body[cfg.blocks[b].start..cfg.blocks[b].end]
                .iter()
                .fold(used, |used, inst| {
                    let (reads, writes) = effects(inst);
                    used | reads | writes
                })
        }) | exits.iter().fold(0, |used, &b| used | live_in[b]);
        let mut free: Vec<_> = REGS
            .into_iter()
            .chain(ARGUMENTS)
            .filter(|&r| region_used & bit(r) == 0)
            .collect();
        // Prefer an already globally unused register when possible, retaining
        // the smaller frame/prologue chosen by the earlier conservative pass.
        // Argument registers are caller-saved and may be reused after parameter
        // setup. They remain the last choice; explicit operand/liveness checks
        // and the call-free-region guard apply to them exactly as to T/S regs.
        free.sort_by_key(|&r| (ARGUMENTS.contains(&r), used & bit(r) != 0, r as u8));
        if free.is_empty() {
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
        choices.push((score, head, members, exits, insert, hot, free));
    }
    choices.sort_by_key(|(score, head, ..)| (std::cmp::Reverse(*score), *head));
    let Some((_, head, members, exits, insert, hot, free)) = choices.into_iter().next() else {
        return None;
    };
    let header = body[cfg.blocks[head].start..cfg.blocks[head].end]
        .iter()
        .find_map(|inst| match inst {
            A::Label(label) => Some(label.clone()),
            _ => None,
        })?;
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
    cleanup_private(&mut changed, &private);
    code.splice(begin..end, changed);
    Some(header)
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

type Equalities = [u32; 32];

#[derive(Clone, Copy)]
struct ZeroTest {
    left: Reg,
    right: Reg,
    // Whether the computed value is zero exactly when left == right.
    zero_when_equal: bool,
}

fn preserve_test(test: ZeroTest, equal: &Equalities, defs: u32) -> Option<ZeroTest> {
    let preserve = |reg: Reg| {
        if defs & bit(reg) == 0 {
            return Some(reg);
        }
        // The operand's old value may still exist in an unchanged copy. Only
        // pre-instruction equality facts qualify; the new destination value
        // must never be mistaken for the value used by the comparison.
        let available = equal[reg as usize] & !defs;
        (available != 0).then(|| {
            let id = if available & bit(Reg::R0) != 0 {
                Reg::R0 as u8
            } else {
                (31 - available.leading_zeros()) as u8
            };
            Reg::from_u8(id).unwrap()
        })
    };
    Some(ZeroTest {
        left: preserve(test.left)?,
        right: preserve(test.right)?,
        ..test
    })
}

/// Fuse local equality tests without changing any register definitions. Dead
/// boolean materialization is then removed by ordinary register liveness; a
/// predicate also used after the branch keeps its original value and code.
fn fuse_comparison_branches(code: &mut [A], cfg: &Cfg) -> bool {
    let mut changed = false;
    for block in &cfg.blocks {
        let mut tests: [Option<ZeroTest>; 32] = [None; 32];
        let mut equal = identity_equalities();
        let mut constants = [None; 32];
        constants[Reg::R0 as usize] = Some(0);
        for inst in &mut code[block.start..block.end] {
            let branch = match &*inst {
                A::Beq(a, b, label) | A::Bne(a, b, label) => {
                    let test = if constants[*b as usize] == Some(0) {
                        tests[*a as usize]
                    } else if constants[*a as usize] == Some(0) {
                        tests[*b as usize]
                    } else {
                        None
                    };
                    test.map(|test| (test, label.clone(), matches!(inst, A::Beq(..))))
                }
                _ => None,
            };
            if let Some((test, label, branch_on_zero)) = branch {
                changed = true;
                *inst = if test.zero_when_equal == branch_on_zero {
                    A::Beq(test.left, test.right, label)
                } else {
                    A::Bne(test.left, test.right, label)
                };
            }
            let next = match *inst {
                A::Xor(dest, left, right) => Some((
                    dest,
                    ZeroTest {
                        left,
                        right,
                        zero_when_equal: true,
                    },
                )),
                A::Sltu(dest, left, right) if constants[left as usize] == Some(0) => Some((
                    dest,
                    tests[right as usize].unwrap_or(ZeroTest {
                        left: right,
                        right: Reg::R0,
                        zero_when_equal: true,
                    }),
                )),
                A::Sltu(dest, left, right) if constants[right as usize] == Some(1) => {
                    let mut test = tests[left as usize].unwrap_or(ZeroTest {
                        left,
                        right: Reg::R0,
                        zero_when_equal: true,
                    });
                    test.zero_when_equal = !test.zero_when_equal;
                    Some((dest, test))
                }
                _ => copied_register(inst).and_then(|(d, s)| tests[s as usize].map(|t| (d, t))),
            };
            let constant = match *inst {
                A::Li(d, value) => Some((d, Some(value))),
                _ => copied_register(inst).map(|(d, s)| (d, constants[s as usize])),
            };
            let defs = effects(inst).1 & !bit(Reg::R0);
            for (r, test) in tests.iter_mut().enumerate() {
                *test = if defs & (1 << r) != 0 {
                    None
                } else {
                    test.and_then(|t| preserve_test(t, &equal, defs))
                };
                if defs & (1 << r) != 0 {
                    constants[r] = None;
                }
            }
            if let Some((dest, test)) = next {
                if dest != Reg::R0 {
                    tests[dest as usize] = preserve_test(test, &equal, defs);
                }
            }
            transfer_equalities(inst, &mut equal);
            if let Some((dest, value)) = constant {
                if dest != Reg::R0 {
                    constants[dest as usize] = value;
                }
            }
        }
    }
    changed
}

fn identity_equalities() -> Equalities {
    std::array::from_fn(|r| 1 << r)
}

fn copied_register(inst: &A) -> Option<(Reg, Reg)> {
    match *inst {
        A::Move(d, s)
        | A::AddI(d, s, 0)
        | A::SubI(d, s, 0)
        | A::Add(d, s, Reg::R0)
        | A::Add(d, Reg::R0, s)
        | A::Sub(d, s, Reg::R0) => Some((d, s)),
        A::Li(d, 0) => Some((d, Reg::R0)),
        _ => None,
    }
}

fn transfer_equalities(inst: &A, equal: &mut Equalities) {
    if copied_register(inst).is_some_and(|(dest, source)| dest == source) {
        return;
    }
    // R0 never changes, including when an instruction names it as an output.
    let defs = effects(inst).1 & !bit(Reg::R0);
    for (r, class) in equal.iter_mut().enumerate() {
        *class = if defs & (1 << r) != 0 {
            1 << r
        } else {
            *class & !defs
        };
    }
    if let Some((dest, source)) = copied_register(inst) {
        if dest != Reg::R0 && dest != source {
            let class = equal[source as usize] | bit(dest);
            for (r, members) in equal.iter_mut().enumerate() {
                if class & (1 << r) != 0 {
                    *members = class;
                }
            }
        }
    }
}

/// Keep only physical-register equalities established on every incoming path.
/// Starting with identity facts prevents a backedge from inventing an equality
/// on the first entry. No memory values or branch outcomes are inferred.
fn propagate_copies(code: &mut [A], cfg: &Cfg) {
    let identity = identity_equalities();
    let mut inputs = vec![identity; cfg.blocks.len()];
    let mut outputs = inputs.clone();
    let mut pending: BTreeSet<_> = cfg.reachable.iter().copied().collect();
    // Bounds compile time even for unusually tangled machine CFGs. An
    // unfinished analysis performs no rewriting at all.
    let mut remaining = cfg.blocks.len().saturating_mul(64);
    while let Some(b) = pending.pop_first() {
        if remaining == 0 {
            return;
        }
        remaining -= 1;
        let mut input = identity;
        if b != 0 {
            let mut preds = cfg.preds[b].iter().filter(|p| cfg.reachable.contains(p));
            if let Some(&first) = preds.next() {
                input = outputs[first];
                for &pred in preds {
                    for (r, class) in input.iter_mut().enumerate() {
                        *class &= outputs[pred][r];
                    }
                }
            }
        }
        inputs[b] = input;
        let mut output = input;
        for inst in &code[cfg.blocks[b].start..cfg.blocks[b].end] {
            transfer_equalities(inst, &mut output);
        }
        if output != outputs[b] {
            outputs[b] = output;
            pending.extend(&cfg.succs[b]);
        }
    }
    for &b in &cfg.reachable {
        let mut equal = inputs[b];
        for inst in &mut code[cfg.blocks[b].start..cfg.blocks[b].end] {
            let aliases = std::array::from_fn(|r| {
                let class = equal[r];
                // Prefer immutable zero, then the higher-numbered register.
                // Promoted S registers then survive temporary copies across
                // loop branches; the choice only affects code quality.
                let representative = if class & bit(Reg::R0) != 0 {
                    Reg::R0 as u8
                } else {
                    (31 - class.leading_zeros()) as u8
                };
                Reg::from_u8(representative).unwrap()
            });
            rewrite_reads(inst, &aliases);
            if copied_register(inst).is_some_and(|(dest, source)| {
                dest == source || dest == Reg::R0 || equal[dest as usize] & bit(source) != 0
            }) {
                *inst = A::Comment("Removed redundant register copy".into());
            }
            transfer_equalities(inst, &mut equal);
        }
    }
}

/// Non-escaping, nonvolatile homes are observable only through their loads.
/// Keep a store if any path reads its old value before a definite overwrite.
/// Unknown reads and calls conservatively observe every home; unknown stores
/// do not prove an overwrite. This also handles zero-trip and outer-loop paths.
fn discard_dead_private_accesses(code: &mut [A], private: &BTreeSet<i16>) {
    if private.is_empty() {
        return;
    }
    let Some(cfg) = Cfg::new(code) else { return };
    let slots = accesses(code, &cfg);
    let (_, _, after) = cfg.liveness(code);
    for i in 0..code.len() {
        if let A::Load(dest, Reg::Sb, _) = code[i] {
            if slots[i].is_some_and(|s| private.contains(&s))
                && (dest == Reg::R0 || after[i] & bit(dest) == 0)
            {
                code[i] = A::Comment("Removed dead private stack load".into());
            }
        }
    }
    let transfer_live = |i: usize, live: &mut BTreeSet<i16>| {
        match code[i] {
            A::Load(..) => {
                if let Some(slot) = slots[i] {
                    if private.contains(&slot) {
                        live.insert(slot);
                    }
                } else {
                    live.extend(private);
                }
            }
            A::Store(..) => {
                if let Some(slot) = slots[i] {
                    live.remove(&slot);
                }
            }
            // Calls, raw asm, indirect control flow, and unrecognized opcodes
            // can observe memory. In particular, don't extend a private-home
            // lifetime proof across a callee's stack activity.
            _ if effects(&code[i]) == (u32::MAX, u32::MAX) => {
                live.extend(private);
            }
            _ => {}
        }
    };
    let mut input = vec![BTreeSet::new(); cfg.blocks.len()];
    let mut output = input.clone();
    let mut pending = cfg.reachable.clone();
    // Every useful update adds a cell to a block's live set. Bound the work
    // and retain all stores if an unusually large graph exhausts this budget.
    let mut remaining = cfg
        .blocks
        .len()
        .saturating_mul(private.len() + 1)
        .saturating_mul(2);
    while let Some(b) = pending.pop_last() {
        if remaining == 0 {
            return;
        }
        remaining -= 1;
        let mut live = BTreeSet::new();
        for &next in &cfg.succs[b] {
            live.extend(&input[next]);
        }
        output[b] = live.clone();
        for i in (cfg.blocks[b].start..cfg.blocks[b].end).rev() {
            transfer_live(i, &mut live);
        }
        if input[b] != live {
            input[b] = live;
            pending.extend(cfg.preds[b].iter().filter(|p| cfg.reachable.contains(p)));
        }
    }
    let mut dead = vec![];
    for &b in &cfg.reachable {
        let mut live = output[b].clone();
        for i in (cfg.blocks[b].start..cfg.blocks[b].end).rev() {
            if matches!(code[i], A::Store(_, Reg::Sb, _))
                && slots[i].is_some_and(|s| private.contains(&s) && !live.contains(&s))
            {
                dead.push(i);
            }
            transfer_live(i, &mut live);
        }
    }
    for i in dead {
        code[i] = A::Comment("Removed dead private stack store".into());
    }
}

#[cfg(test)]
fn cleanup(code: &mut Vec<A>) {
    cleanup_private(code, &BTreeSet::new());
}

fn cleanup_private(code: &mut Vec<A>, private: &BTreeSet<i16>) {
    for _ in 0..4 {
        let Some(cfg) = Cfg::new(code) else {
            return;
        };
        propagate_copies(code, &cfg);
        let (_, live_out, _) = cfg.liveness(code);
        for (b, block) in cfg.blocks.iter().enumerate() {
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
                        // A loop counter copied to its surviving register
                        // should use that register immediately, so the update
                        // and copy can coalesce. Otherwise use the source and
                        // discard transient reload copies, including cached
                        // argument registers whose numbers are lower than T's.
                        if a != Reg::R0 && live_out[b] & bit(d) != 0 && live_out[b] & bit(a) == 0 {
                            aliases[a as usize] = d;
                        } else {
                            aliases[d as usize] = a;
                        }
                    }
                }
                if let Some((d, v)) = new_constant {
                    if d != Reg::R0 {
                        constants[d as usize] = v;
                    }
                }
            }
        }
        fuse_comparison_branches(code, &cfg);
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
        discard_dead_private_accesses(code, private);
    }
}

#[cfg(test)]
mod tests;
