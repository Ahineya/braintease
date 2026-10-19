//! Closed-world callback identities without cloning procedures or changing their
//! ownership protocol. Conflicting/escaped values use the ordinary dynamic call.
use super::*;
use std::collections::{BTreeMap, HashMap, VecDeque};

mod facts;
use facts::{Fact, State};

pub(super) fn resolve_captures(procs: &mut [Proc]) -> Result<(), CompileError> {
    let captures: HashMap<_, Vec<_>> = procs
        .iter()
        .map(|p| {
            (
                p.name.clone(),
                p.captures.iter().map(|c| c.parent).collect(),
            )
        })
        .collect();
    for proc in procs {
        for insn in proc.segs.iter_mut().flat_map(|s| &mut s.insns) {
            if let Insn::Closure {
                func,
                captures: reads,
                ..
            } = insn
            {
                let parents = captures.get(func).ok_or_else(|| {
                    CompileError::plain(format!("closure body `{func}` is missing during lowering"))
                })?;
                *reads = parents
                    .iter()
                    .map(|parent| {
                        proc.binding_slots
                            .get(*parent as usize)
                            .copied()
                            .ok_or_else(|| {
                                CompileError::plain(format!(
                                    "closure `{func}` names a missing capture in `{}`",
                                    proc.name
                                ))
                            })
                    })
                    .collect::<Result<_, _>>()?;
            }
        }
    }
    Ok(())
}

pub(super) fn analyze(procs: &[Proc]) -> Vec<BTreeMap<usize, String>> {
    let names: HashMap<_, _> = procs
        .iter()
        .enumerate()
        .map(|(i, p)| (p.name.as_str(), i))
        .collect();
    let disabled: Vec<Vec<_>> = procs
        .iter()
        .map(|p| {
            let mut slots: Vec<_> = p.slots.iter().map(|s| s.shared).collect();
            for i in p.segs.iter().flat_map(|s| &s.insns) {
                if let Insn::Borrow { place, .. } = i {
                    slots[place.root] = true;
                }
            }
            slots
        })
        .collect();
    let mut incoming: Vec<Option<State>> = procs
        .iter()
        .map(|p| (p.name == "main" || p.c_export).then(|| vec![None; p.slots.len()]))
        .collect();
    // A reached input starts with the first observed call. Further observations
    // can only lose facts. Roots and closure parameters admit unknown callers;
    // no analysis result relies on discovering just one convenient call site.
    loop {
        let mut changed = false;
        for (index, proc) in procs.iter().enumerate() {
            let Some(initial) = incoming[index].clone() else {
                continue;
            };
            let result = procedure(proc, &initial, procs, &names, &disabled[index]);
            for (callee, mut state) in result.observations {
                for (slot, disabled) in disabled[callee].iter().enumerate() {
                    if *disabled {
                        state[slot] = None;
                    }
                }
                match &mut incoming[callee] {
                    None => {
                        incoming[callee] = Some(state);
                        changed = true;
                    }
                    Some(old) => changed |= facts::join(old, &state),
                }
            }
        }
        if !changed {
            break;
        }
    }
    procs
        .iter()
        .enumerate()
        .map(|(index, proc)| {
            incoming[index]
                .as_ref()
                .map(|state| {
                    procedure(proc, state, procs, &names, &disabled[index])
                        .calls
                        .into_iter()
                        .map(|(segment, callee)| (segment, procs[callee].name.clone()))
                        .collect()
                })
                .unwrap_or_default()
        })
        .collect()
}

struct Analysis {
    calls: BTreeMap<usize, usize>,
    observations: Vec<(usize, State)>,
}

fn observe(result: &mut Analysis, callee: usize, args: &[usize], state: &State, procs: &[Proc]) {
    let mut input = vec![None; procs[callee].slots.len()];
    for (&arg, &param) in args.iter().zip(&procs[callee].param_slots) {
        input[param] = state[arg].clone();
    }
    result.observations.push((callee, input));
}

fn procedure(
    proc: &Proc,
    initial: &State,
    procs: &[Proc],
    names: &HashMap<&str, usize>,
    disabled: &[bool],
) -> Analysis {
    let mut result = Analysis {
        calls: BTreeMap::new(),
        observations: Vec::new(),
    };
    let mut inputs = vec![None; proc.segs.len()];
    inputs[0] = Some(initial.clone());
    let mut queue = VecDeque::from([0]);
    while let Some(index) = queue.pop_front() {
        let mut state = inputs[index].clone().unwrap();
        let segment = &proc.segs[index];
        for insn in &segment.insns {
            match insn {
                Insn::Closure { func, captures, .. } => {
                    let callee = names[func.as_str()];
                    // Merely making a function value allows unknown invocation
                    // arguments, but its immutable captures are explicit copies.
                    let mut input = vec![None; procs[callee].slots.len()];
                    for (&src, cap) in captures.iter().zip(&procs[callee].captures) {
                        if !cap.shared {
                            input[cap.slot] = state[src].clone();
                        }
                    }
                    result.observations.push((callee, input));
                }
                Insn::AsyncNew { body, args, .. } => {
                    observe(&mut result, names[body.as_str()], args, &state, procs)
                }
                _ => {}
            }
            facts::instruction(insn, &mut state, proc, names);
            for (slot, disabled) in disabled.iter().enumerate() {
                if *disabled {
                    state[slot] = None;
                }
            }
        }
        let successors = match segment.term.as_ref().unwrap() {
            Term::Call {
                callee,
                args,
                dest,
                next,
            } => {
                observe(&mut result, names[callee.as_str()], args, &state, procs);
                state[*dest] = None;
                vec![*next]
            }
            Term::Tail { callee, args } => {
                observe(&mut result, names[callee.as_str()], args, &state, procs);
                vec![]
            }
            Term::Apply {
                slot, dest, next, ..
            } => {
                result.calls.remove(&index);
                if let Some(Fact::Function(callee)) = &state[*slot] {
                    result.calls.insert(index, *callee);
                    // Closure construction already admitted arbitrary arguments
                    // and recorded the environment. Re-observing it without its
                    // captures here would erase those immutable facts.
                }
                state[*dest] = None;
                vec![*next]
            }
            Term::AsyncPoll { dest, next, .. } => {
                state[*dest] = None;
                vec![*next]
            }
            Term::Jump(next) | Term::Suspend { next, .. } => vec![*next],
            Term::If {
                then_seg, else_seg, ..
            }
            | Term::Tag {
                then_seg, else_seg, ..
            }
            | Term::Lit {
                then_seg, else_seg, ..
            } => vec![*then_seg, *else_seg],
            Term::Return | Term::Fail => vec![],
        };
        for next in successors {
            let changed = match &mut inputs[next] {
                None => {
                    inputs[next] = Some(state.clone());
                    true
                }
                Some(old) => facts::join(old, &state),
            };
            if changed {
                queue.push_back(next);
            }
        }
    }
    result
}
