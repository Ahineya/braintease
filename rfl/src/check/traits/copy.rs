//! Structural proof for std/value::Copy. This marker cannot waive linearity.
use super::*;
impl Checker<'_> {
    pub(in crate::check) fn is_copy_trait(&self, name: &str) -> bool {
        self.traits.iter().any(|tr| {
            tr.name == name
                && self.source.get(tr.name_span.start..tr.name_span.end) == Some("Copy")
                && self
                    .trait_scopes
                    .iter()
                    .any(|s| s.core && s.span.contains(tr.span.start))
        })
    }

    pub(super) fn copy_value(&self, bound: &str, ty: Ty, seen: &mut HashSet<Ty>) -> bool {
        if let Some((_, bounds)) = self.opaque.get(&ty) {
            return bounds.iter().any(|b| b == bound);
        }
        // This is an AND over every reachable stored field. Visit each type
        // once, including completed branches of a shared DAG. Any rejecting
        // field immediately propagates to the root; recursive backedges add
        // no requirements beyond the definition already being visited.
        if !seen.insert(ty) {
            return true;
        }
        match ty {
            Ty::Owned(_) | Ty::ErasedOwner | Ty::Ref(_) | Ty::Constructor(_) => false,
            Ty::Func(id) => !self.funcs[id as usize].once,
            Ty::Struct { id, .. } => self.structs[id as usize]
                .fields
                .iter()
                .all(|(_, t)| self.copy_value(bound, *t, seen)),
            Ty::Tuple { id, .. } => self.tuples[id as usize]
                .iter()
                .all(|t| self.copy_value(bound, *t, seen)),
            Ty::Adt(id) => {
                let adt = &self.adts[id as usize];
                !adt.ctors.is_empty()
                    && adt
                        .ctors
                        .iter()
                        .all(|c| c.fields.iter().all(|t| self.copy_value(bound, *t, seen)))
            }
            Ty::Vec(id) => self.copy_value(bound, self.vec_elems[id as usize], seen),
            Ty::Slice(id) => self.copy_value(bound, self.slice_elems[id as usize], seen),
            Ty::Array { id, .. } => self.copy_value(bound, self.arrays[id as usize].elem, seen),
            // Signatures of reusable functions, raw pointers and C function
            // pointers do not store their argument/result types.
            _ => true,
        }
    }
}
