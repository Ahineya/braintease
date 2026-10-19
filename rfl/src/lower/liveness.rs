//! Scalar values needed across resumable entries. Locally defined temporaries
//! need neither an initial heap read nor publication at every later call.
use std::collections::HashSet;

use super::*;

pub(super) struct FrameLiveness {
    /// Existing values that the caller must publish before storing a resume ID.
    pub at_resume: Vec<HashSet<usize>>,
    /// Values read before definition at each segment. Unlike
    /// publication, this includes results written into the frame by a callee.
    pub at_entry: Vec<HashSet<usize>>,
}

pub(super) fn frame_liveness(proc: &Proc) -> FrameLiveness {
    let mut uses = vec![HashSet::new(); proc.segs.len()];
    let mut defs = uses.clone();
    let mut successors = vec![Vec::new(); proc.segs.len()];
    for (index, seg) in proc.segs.iter().enumerate() {
        for insn in &seg.insns {
            for slot in operands(insn) {
                read(slot, &defs[index], &mut uses[index]);
            }
            for slot in destinations(insn) {
                defs[index].insert(slot);
            }
        }
        match seg.term.as_ref().unwrap() {
            Term::Jump(next) | Term::Suspend { next, .. } => successors[index].push(*next),
            Term::AsyncPoll {
                future,
                context,
                dest,
                next,
            } => {
                read(*future, &defs[index], &mut uses[index]);
                read(*context, &defs[index], &mut uses[index]);
                defs[index].insert(*dest);
                successors[index].push(*next);
            }
            Term::Call {
                args, dest, next, ..
            }
            | Term::Apply {
                args, dest, next, ..
            } => {
                for slot in args {
                    read(*slot, &defs[index], &mut uses[index]);
                }
                if let Some(Term::Apply { slot, .. }) = &seg.term {
                    read(*slot, &defs[index], &mut uses[index]);
                }
                defs[index].insert(*dest);
                successors[index].push(*next);
            }
            Term::Tail { args, .. } => {
                for slot in args {
                    read(*slot, &defs[index], &mut uses[index]);
                }
            }
            Term::If {
                cond,
                then_seg,
                else_seg,
            } => {
                read(*cond, &defs[index], &mut uses[index]);
                successors[index].extend([*then_seg, *else_seg]);
            }
            Term::Tag {
                slot,
                then_seg,
                else_seg,
                ..
            }
            | Term::Lit {
                slot,
                then_seg,
                else_seg,
                ..
            } => {
                read(*slot, &defs[index], &mut uses[index]);
                successors[index].extend([*then_seg, *else_seg]);
            }
            Term::Return | Term::Fail => {}
        }
    }
    let mut live = uses.clone();
    loop {
        let mut changed = false;
        for index in (0..proc.segs.len()).rev() {
            let mut next = uses[index].clone();
            for successor in &successors[index] {
                next.extend(live[*successor].difference(&defs[index]).copied());
            }
            if next != live[index] {
                live[index] = next;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    // Indexed by the stored resume ID, so emission can query the publication
    // point without relying on IR labels or on the order blocks were emitted.
    let mut at_resume = vec![HashSet::new(); proc.segs.len()];
    for seg in &proc.segs {
        if let Some(Term::Suspend { next, .. }) = &seg.term {
            at_resume[*next].extend(live[*next].iter().copied());
        }
        if let Some(
            Term::Call { dest, next, .. }
            | Term::Apply { dest, next, .. }
            | Term::AsyncPoll { dest, next, .. },
        ) = &seg.term
        {
            // The callee writes its result directly into the heap frame.
            at_resume[*next].extend(live[*next].iter().filter(|slot| *slot != dest).copied());
        }
    }
    FrameLiveness {
        at_resume,
        at_entry: live,
    }
}

fn read(slot: usize, defs: &HashSet<usize>, uses: &mut HashSet<usize>) {
    if !defs.contains(&slot) {
        uses.insert(slot);
    }
}
