//! Only identities of immutable function values and their inline/container
//! fields are tracked. Pointer dereferences and opaque results stay unknown.
use super::*;

#[derive(Clone, PartialEq, Eq)]
pub(super) enum Fact {
    Function(usize),
    Fields(Arc<BTreeMap<i64, Fact>>),
}
pub(super) type State = Vec<Option<Fact>>;

pub(super) fn join(old: &mut State, new: &State) -> bool {
    let mut changed = false;
    for (a, b) in old.iter_mut().zip(new) {
        if a.is_some() && a != b {
            *a = None;
            changed = true;
        }
    }
    changed
}

fn fields(values: impl IntoIterator<Item = (i64, Option<Fact>)>) -> Option<Fact> {
    let values: BTreeMap<_, _> = values
        .into_iter()
        .filter_map(|(o, v)| v.map(|v| (o, v)))
        .collect();
    (!values.is_empty()).then(|| Fact::Fields(Arc::new(values)))
}

pub(super) fn instruction(
    insn: &Insn,
    state: &mut State,
    proc: &Proc,
    names: &HashMap<&str, usize>,
) {
    let value = match insn {
        Insn::Closure { dest, func, .. } => {
            Some((*dest, Some(Fact::Function(names[func.as_str()]))))
        }
        Insn::Copy { dest, src } | Insn::Move { dest, src } => Some((*dest, state[*src].clone())),
        Insn::Record {
            dest,
            fields: slots,
        } => {
            let mut offset = 0;
            let parts = slots.iter().map(|slot| {
                let at = offset;
                offset += proc.slots[*slot].ty.cells();
                (at, state[*slot].clone())
            });
            Some((*dest, fields(parts)))
        }
        Insn::Make {
            dest,
            fields: slots,
            ..
        } => {
            let mut offset = OBJ_FIELDS;
            let parts = slots.iter().map(|(slot, cells)| {
                let at = offset;
                offset += cells;
                (at, state[*slot].clone())
            });
            Some((*dest, fields(parts)))
        }
        Insn::Read {
            dest, src, offset, ..
        }
        | Insn::Take { dest, src, offset } => {
            let value = match &state[*src] {
                Some(Fact::Fields(fields)) => fields.get(offset).cloned(),
                _ => None,
            };
            Some((*dest, value))
        }
        _ => None,
    };
    for slot in destinations(insn) {
        state[slot] = None;
    }
    if let Some((dest, value)) = value {
        state[dest] = value;
    }
}
