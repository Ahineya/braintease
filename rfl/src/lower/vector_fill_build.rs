//! Fold an immediately initialized vector's constant push loop into a bulk fill.
use super::*;

impl FnBuild {
    pub(super) fn fold_vector_fill(&mut self, start: &TExpr, end: &TExpr, body: &TBlock) -> bool {
        let (TExprKind::Int(start), TExprKind::Int(end)) = (&start.kind, &end.kind) else {
            return false;
        };
        let Some(count) = end.checked_sub(*start) else {
            return false;
        };
        let Ok(len) = u32::try_from(count.max(0)) else {
            return false;
        };
        let expr = match (body.stmts.as_slice(), body.tail.as_ref()) {
            ([TStmt::Expr(e)], None) | ([], Some(e)) => e,
            _ => return false,
        };
        let TExprKind::Push {
            binding,
            offset: 0,
            value,
            elem,
        } = &expr.kind
        else {
            return false;
        };
        let TExprKind::Int(value) = value.kind else {
            return false;
        };
        // memset fills VM cells through C unsigned char, so preserve its range.
        if elem.cells() != 1 || !(0..=255).contains(&value) {
            return false;
        }
        let dest = self.binding_slot[*binding as usize];
        if self.slots[dest].shared {
            return false;
        }
        // Require a fresh empty owner in this block. Passing through arbitrary
        // operations could expose an alias or mutate the vector indirectly.
        for insn in self.segs[self.cur].insns.iter_mut().rev() {
            match insn {
                Insn::VecLit {
                    dest: slot, elems, ..
                } if *slot == dest && elems.is_empty() => {
                    if len != 0 {
                        *insn = Insn::VecFill { dest, len, value };
                    }
                    return true;
                }
                Insn::Const { slot, .. } if *slot != dest => {}
                _ => return false,
            }
        }
        false
    }
}
