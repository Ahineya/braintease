//! Move checking is a separate control-flow pass over fully specialized types.
//! Ordinary values keep copy semantics; owners and aggregates containing them
//! require an explicit transfer. References never become resource owners.
use crate::{ast::BinOp, error::CompileError, ty::Ty, typed::*};
use std::collections::{HashMap, HashSet};

#[derive(Default)]
pub(crate) struct ResourceTypes {
    once_functions: HashSet<u32>,
    /// Destruction tracking only; never used to authorize copies or transfers.
    shared_cleanup: bool,
    structs: Vec<bool>,
    tuples: Vec<bool>,
    adts: Vec<bool>,
    vectors: Vec<bool>,
    arrays: Vec<bool>,
    slices: Vec<bool>,
}
impl ResourceTypes {
    pub fn new(module: &TModule) -> Self {
        Self::build(module, false)
    }

    /// Shared handles and closures retaining them have observable last-drop
    /// cleanup, even though they remain copyable. Keep this proof independent
    /// of move checking and call argument transfer decisions.
    pub fn for_scope_cleanup(module: &TModule) -> Self {
        let shared = module
            .functions
            .iter()
            .any(|f| f.ret == Ty::SharedOwner || f.bindings.contains(&Ty::SharedOwner))
            || module
                .adts
                .iter()
                .flat_map(|a| &a.ctors)
                .flat_map(|c| &c.fields)
                .chain(module.structs.iter().flatten())
                .chain(module.tuples.iter().flatten())
                .chain(&module.collections.vectors)
                .chain(&module.collections.slices)
                .chain(&module.collections.arrays)
                .chain(&module.collections.owners)
                .any(|t| *t == Ty::SharedOwner);
        Self::build(module, shared)
    }

    fn build(module: &TModule, shared_cleanup: bool) -> Self {
        let mut result = Self {
            once_functions: module.once_functions.iter().copied().collect(),
            shared_cleanup,
            structs: vec![false; module.structs.len()],
            tuples: vec![false; module.tuples.len()],
            adts: vec![false; module.adts.len()],
            vectors: vec![false; module.collections.vectors.len()],
            arrays: vec![false; module.collections.arrays.len()],
            slices: vec![false; module.collections.slices.len()],
        };
        // Monotone propagation handles recursive ADTs without a recursion limit.
        loop {
            let mut changed = false;
            for (id, fields) in module.structs.iter().enumerate() {
                if !result.structs[id] && fields.iter().any(|t| result.contains(*t)) {
                    result.structs[id] = true;
                    changed = true;
                }
            }
            for (id, fields) in module.tuples.iter().enumerate() {
                if !result.tuples[id] && fields.iter().any(|t| result.contains(*t)) {
                    result.tuples[id] = true;
                    changed = true;
                }
            }
            for (id, adt) in module.adts.iter().enumerate() {
                if !result.adts[id]
                    && adt
                        .ctors
                        .iter()
                        .flat_map(|c| &c.fields)
                        .any(|t| result.contains(*t))
                {
                    result.adts[id] = true;
                    changed = true;
                }
            }
            for (id, elem) in module.collections.vectors.iter().enumerate() {
                if !result.vectors[id] && result.contains(*elem) {
                    result.vectors[id] = true;
                    changed = true;
                }
            }
            for (id, elem) in module.collections.arrays.iter().enumerate() {
                if !result.arrays[id] && result.contains(*elem) {
                    result.arrays[id] = true;
                    changed = true;
                }
            }
            for (id, elem) in module.collections.slices.iter().enumerate() {
                if !result.slices[id] && result.contains(*elem) {
                    result.slices[id] = true;
                    changed = true;
                }
            }
            if !changed {
                return result;
            }
        }
    }
    pub fn contains(&self, ty: Ty) -> bool {
        match ty {
            Ty::Owned(_) | Ty::ErasedOwner => true,
            Ty::SharedOwner => self.shared_cleanup,
            // A reusable callable may retain shared owners in its environment.
            Ty::Func(id) => self.shared_cleanup || self.once_functions.contains(&id),
            Ty::Struct { id, .. } => self.structs[id as usize],
            Ty::Tuple { id, .. } => self.tuples[id as usize],
            Ty::Adt(id) => self.adts[id as usize],
            Ty::Vec(id) => self.vectors[id as usize],
            Ty::Slice(id) => self.slices[id as usize],
            Ty::Array { id, .. } => self.arrays[id as usize],
            _ => false,
        }
    }
}

type State = HashMap<u32, usize>;
#[derive(Default)]
struct Loop {
    breaks: Vec<State>,
    continues: Vec<State>,
}
struct Check<'a> {
    source: &'a str,
    types: &'a ResourceTypes,
    loops: Vec<Loop>,
    known: std::collections::HashSet<u32>,
    closures: &'a HashMap<String, Vec<Capture>>,
}
#[derive(Clone, Copy)]
enum Use {
    Value,
    Place,
}

pub(crate) fn check(source: &str, module: &TModule) -> Result<(), CompileError> {
    check_with_named_functions(source, module, std::iter::empty())
}

/// Declaration checking contains only the bodies instantiated so far. Named
/// functions have no lexical captures, including those not checked until the
/// concrete pass. Lifted lambdas must always supply their actual capture lists.
pub(crate) fn check_with_named_functions<'a>(
    source: &str,
    module: &TModule,
    named: impl IntoIterator<Item = &'a str>,
) -> Result<(), CompileError> {
    let types = ResourceTypes::new(module);
    if module.collections.slices.iter().any(|t| types.contains(*t)) {
        return Err(CompileError::at(source,0,"owning slices cannot share move-only resource elements; borrow an element or keep its vector owner"));
    }
    let mut closures: HashMap<_, _> = module
        .functions
        .iter()
        .map(|f| (f.name.clone(), f.captures.clone()))
        .collect();
    for name in named {
        closures.entry(name.to_owned()).or_default();
    }
    for function in &module.functions {
        if let Some(cap) = function
            .captures
            .iter()
            .find(|c| !c.transfer && types.contains(c.ty))
        {
            return Err(CompileError::at(source,cap.span,"a copyable closure cannot capture a move-only resource; pass a temporary reference instead"));
        }
        Check {
            source,
            types: &types,
            loops: vec![],
            closures: &closures,
            known: function
                .params
                .iter()
                .copied()
                .chain(function.captures.iter().map(|c| c.local))
                .collect(),
        }
        .block(&function.body, &mut State::new())?;
    }
    Ok(())
}
impl Check<'_> {
    fn error<T>(&self, span: usize, message: &str) -> Result<T, CompileError> {
        Err(CompileError::at(self.source, span, message))
    }
    fn live(&self, id: u32, span: usize, state: &State) -> Result<(), CompileError> {
        if state.contains_key(&id) {
            self.error(
                span,
                "use of a moved value; reinitialize the binding before using it again",
            )
        } else {
            Ok(())
        }
    }
    fn block(&mut self, block: &TBlock, state: &mut State) -> Result<bool, CompileError> {
        for stmt in &block.stmts {
            if !self.stmt(stmt, state)? {
                return Ok(false);
            }
        }
        if let Some(tail) = &block.tail {
            self.expr(tail, Use::Value, state)
        } else {
            Ok(true)
        }
    }
    fn define(pat: &TPat, state: &mut State) {
        match pat {
            TPat::Bind(id) => {
                state.remove(id);
            }
            TPat::Ctor { fields, .. } => {
                for f in fields {
                    Self::define(&f.pat, state)
                }
            }
            TPat::Product { fields } => {
                for f in fields {
                    Self::define(&f.pat, state)
                }
            }
            _ => {}
        }
    }
    fn stmt(&mut self, stmt: &TStmt, state: &mut State) -> Result<bool, CompileError> {
        match stmt {
            TStmt::Let { binding, init }
            | TStmt::Assign {
                binding,
                value: init,
            } => {
                if !self.expr(init, Use::Value, state)? {
                    return Ok(false);
                }
                state.remove(binding);
                self.known.insert(*binding);
            }
            TStmt::Destructure { pat, init } => {
                if !self.expr(init, Use::Value, state)? {
                    return Ok(false);
                }
                Self::define(pat, state);
                self.known.extend(pattern_bindings(pat));
            }
            TStmt::FieldAssign { binding, value, .. } => {
                self.live(*binding, 0, state)?;
                if !self.expr(value, Use::Value, state)? {
                    return Ok(false);
                }
                self.live(*binding, 0, state)?;
            }
            TStmt::IndexAssign {
                binding,
                index,
                value,
                ..
            } => {
                self.live(*binding, 0, state)?;
                if !self.expr(index, Use::Value, state)? {
                    return Ok(false);
                }
                if !self.expr(value, Use::Value, state)? {
                    return Ok(false);
                }
                self.live(*binding, 0, state)?;
            }
            TStmt::PtrStore {
                ptr, index, value, ..
            } => {
                if !self.expr(ptr, Use::Value, state)? {
                    return Ok(false);
                }
                if let Some(i) = index {
                    if !self.expr(i, Use::Value, state)? {
                        return Ok(false);
                    }
                }
                return self.expr(value, Use::Value, state);
            }
            TStmt::Expr(e) => return self.expr(e, Use::Value, state),
            TStmt::Return(e) => {
                if let Some(e) = e {
                    self.expr(e, Use::Value, state)?;
                }
                return Ok(false);
            }
            TStmt::Break => {
                self.loops.last_mut().unwrap().breaks.push(state.clone());
                return Ok(false);
            }
            TStmt::Continue => {
                self.loops.last_mut().unwrap().continues.push(state.clone());
                return Ok(false);
            }
        }
        Ok(true)
    }
    fn values(&mut self, values: &[TExpr], state: &mut State) -> Result<bool, CompileError> {
        for e in values {
            if !self.expr(e, Use::Value, state)? {
                return Ok(false);
            }
        }
        Ok(true)
    }
    fn iteration(
        &mut self,
        body: &TBlock,
        extract: Option<&TStmt>,
        entry: State,
        state: &mut State,
    ) -> Result<(), CompileError> {
        let outer = self.known.clone();
        self.loops.push(Loop::default());
        let mut body_state = state.clone();
        let fall = if let Some(e) = extract {
            self.stmt(e, &mut body_state)?
        } else {
            true
        };
        let fall = fall && self.block(body, &mut body_state)?;
        let mut paths = self.loops.pop().unwrap();
        if fall {
            paths.continues.push(body_state);
        }
        for back in paths.continues {
            if let Some((_, span)) = back
                .iter()
                .find(|(id, _)| outer.contains(id) && !entry.contains_key(id))
            {
                return self.error(*span,"a value is moved in a repeating loop; reinitialize it before the next iteration");
            }
        }
        for exit in paths.breaks {
            state.extend(exit);
        }
        Ok(())
    }
    fn expr(&mut self, e: &TExpr, usage: Use, state: &mut State) -> Result<bool, CompileError> {
        use TExprKind::*;
        match &e.kind {
            Expect { value, message, .. } => {
                if !self.expr(value, Use::Value, state)? {
                    return Ok(false);
                }
                return self.expr(message, Use::Value, state);
            }
            Await {
                owner,
                binding,
                poll,
            } => {
                if !self.expr(owner, Use::Value, state)? {
                    return Ok(false);
                }
                state.remove(binding);
                self.known.insert(*binding);
                return self.expr(poll, Use::Value, state);
            }
            AsyncNew { args, .. } => return self.values(args, state),
            AsyncPoll { future, context } => {
                if !self.expr(future, Use::Value, state)? {
                    return Ok(false);
                }
                return self.expr(context, Use::Value, state);
            }
            AsyncCancel { future } => return self.expr(future, Use::Value, state),
            Name(id, span) => {
                self.live(*id, *span, state)?;
                if matches!(usage, Use::Value) && self.types.contains(e.ty) && *span != 0 {
                    return self.error(*span,"a move-only resource cannot be copied; use move(value) to transfer ownership");
                }
            }
            Move { binding, span } => {
                self.live(*binding, *span, state)?;
                state.insert(*binding, *span);
            }
            Borrow {
                binding,
                span,
                projections,
                ..
            } => {
                self.live(*binding, *span, state)?;
                for p in projections {
                    if let TPlaceStep::Index { index, .. } = p {
                        if !self.expr(index, Use::Value, state)? {
                            return Ok(false);
                        }
                    }
                }
            }
            Owned { value, cleanup } => {
                if !self.expr(value, Use::Value, state)? {
                    return Ok(false);
                }
                return self.expr(cleanup, Use::Value, state);
            }
            Replace { place, value } => {
                if !self.expr(place, Use::Value, state)? {
                    return Ok(false);
                }
                return self.expr(value, Use::Value, state);
            }
            EraseOwner(x)
            | RawBorrow(x)
            | IntoInner(x)
            | Drop(x)
            | Unary { expr: x, .. }
            | Cast { expr: x, .. }
            | StrFromChar(x)
            | StrFromByte(x)
            | Putc(x)
            | Print { value: x, .. }
            | Try { expr: x, .. } => {
                return self.expr(x, Use::Value, state);
            }
            Field { base, .. } => {
                if matches!(usage, Use::Value) && self.types.contains(e.ty) {
                    return self.error(0,"cannot copy a resource field or borrowed resource; borrow it or destructure move(container)");
                }
                return self.expr(base, Use::Place, state);
            }
            Index { base, index, .. } => {
                if matches!(usage, Use::Value) && self.types.contains(e.ty) {
                    return self.error(
                        0,
                        "cannot copy a move-only element; borrow it or use pop() to transfer it",
                    );
                }
                if !self.expr(base, Use::Place, state)? {
                    return Ok(false);
                }
                return self.expr(index, Use::Value, state);
            }
            Get {
                vec, index, elem, ..
            } => {
                if self.types.contains(*elem) {
                    return self.error(
                        0,
                        "get() cannot copy a move-only element; borrow the indexed element instead",
                    );
                }
                if !self.expr(vec, Use::Place, state)? {
                    return Ok(false);
                }
                return self.expr(index, Use::Value, state);
            }
            Binary { op, lhs, rhs } => {
                if !self.expr(lhs, Use::Value, state)? {
                    return Ok(false);
                }
                if matches!(op, BinOp::And | BinOp::Or) {
                    let mut branch = state.clone();
                    if self.expr(rhs, Use::Value, &mut branch)? {
                        state.extend(branch);
                    }
                } else {
                    return self.expr(rhs, Use::Value, state);
                }
            }
            StrConcat { lhs, rhs } => {
                if !self.expr(lhs, Use::Value, state)? {
                    return Ok(false);
                }
                return self.expr(rhs, Use::Value, state);
            }
            Call { args, .. }
            | Extern { args, .. }
            | Ctor { args, .. }
            | Record { fields: args }
            | VecLit { elems: args, .. }
            | ArrayLit { elems: args, .. } => return self.values(args, state),
            Apply { callee, args, .. } | CApply { callee, args } => {
                if !self.expr(callee, Use::Value, state)? {
                    return Ok(false);
                }
                return self.values(args, state);
            }
            If {
                cond,
                then_body,
                else_body,
            } => {
                if !self.expr(cond, Use::Value, state)? {
                    return Ok(false);
                }
                let mut left = state.clone();
                let mut right = state.clone();
                let a = self.block(then_body, &mut left)?;
                let b = if let Some(e) = else_body {
                    self.expr(e, usage, &mut right)?
                } else {
                    true
                };
                state.clear();
                if a {
                    state.extend(left);
                }
                if b {
                    state.extend(right);
                }
                return Ok(a || b);
            }
            Block(b) => {
                if e.ty.is_ref()
                    && b.stmts
                        .iter()
                        .any(|s| matches!(s,TStmt::Let{init,..} if self.types.contains(init.ty)))
                {
                    return self.error(
                        0,
                        "a resource temporary needs a named owner before borrowing its receiver",
                    );
                }
                return self.block(b, state);
            }
            Match { scrutinee, arms } => {
                let borrowed = matches!(&scrutinee.kind,Field{base,..} if base.ty.is_ref());
                if self.types.contains(scrutinee.ty)
                    && !borrowed
                    && arms.iter().any(|a| a.guard.is_some())
                {
                    return self.error(
                        0,
                        "guarded resource matches must borrow their scrutinee; use match &value",
                    );
                }
                if !self.expr(
                    scrutinee,
                    if borrowed { Use::Place } else { Use::Value },
                    state,
                )? {
                    return Ok(false);
                }
                let entry = state.clone();
                let mut falls = false;
                state.clear();
                for arm in arms {
                    let mut branch = entry.clone();
                    Self::define(&arm.pat, &mut branch);
                    self.known.extend(pattern_bindings(&arm.pat));
                    if let Some(g) = &arm.guard {
                        let before = branch.clone();
                        if !self.expr(g, Use::Value, &mut branch)? {
                            continue;
                        }
                        if let Some((_, span)) =
                            branch.iter().find(|(id, _)| !before.contains_key(id))
                        {
                            return self.error(*span,"a match guard cannot move a value; transfer it in the arm body instead");
                        }
                    }
                    if self.block(&arm.body, &mut branch)? {
                        state.extend(branch);
                        falls = true;
                    }
                }
                return Ok(falls);
            }
            While { cond, body } => {
                let entry = state.clone();
                if !self.expr(cond, Use::Value, state)? {
                    return Ok(false);
                }
                self.iteration(body, None, entry, state)?;
            }
            ForIter {
                start,
                end,
                extract,
                body,
                ..
            } => {
                if !self.expr(start, Use::Value, state)? {
                    return Ok(false);
                }
                if !self.expr(end, Use::Value, state)? {
                    return Ok(false);
                }
                self.iteration(body, extract.as_deref(), state.clone(), state)?;
            }
            Len { vec } | Empty { vec } => return self.expr(vec, Use::Place, state),
            Clear { binding, .. } | Pop { binding, .. } => self.live(*binding, 0, state)?,
            Push { binding, value, .. } => {
                self.live(*binding, 0, state)?;
                if !self.expr(value, Use::Value, state)? {
                    return Ok(false);
                }
                self.live(*binding, 0, state)?;
            }
            Slice {
                base, start, end, ..
            }
            | StrSlice {
                base, start, end, ..
            } => {
                if !self.expr(base, Use::Place, state)? {
                    return Ok(false);
                }
                if !self.expr(start, Use::Value, state)? {
                    return Ok(false);
                }
                return self.expr(end, Use::Value, state);
            }
            ToVec { src, .. } => return self.expr(src, Use::Value, state),
            Deref { ptr, index, .. } => {
                if matches!(usage, Use::Value) && self.types.contains(e.ty) {
                    return self.error(
                        0,
                        "cannot copy a move-only resource through a reference; borrow it instead",
                    );
                }
                if !self.expr(ptr, Use::Value, state)? {
                    return Ok(false);
                }
                if let Some(i) = index {
                    if !self.expr(i, Use::Value, state)? {
                        return Ok(false);
                    }
                }
            }
            PtrOffset { ptr, index, .. } => {
                if !self.expr(ptr, Use::Value, state)? {
                    return Ok(false);
                }
                if !self.expr(index, Use::Value, state)? {
                    return Ok(false);
                }
            }
            PtrAddr { ptr } | PtrBank { ptr } | PtrNull { ptr } => {
                return self.expr(ptr, Use::Value, state)
            }
            Closure { func } => {
                let captures = self.closures.get(func).ok_or_else(|| {
                    CompileError::at(
                        self.source,
                        0,
                        format!("missing capture metadata for function `{func}`"),
                    )
                })?;
                for cap in captures {
                    self.live(cap.parent, cap.span, state)?;
                    if cap.transfer {
                        state.insert(cap.parent, cap.span);
                    }
                }
            }
            Int(_)
            | Float(_)
            | Bool(_)
            | CStr(_)
            | Str(_)
            | Char(_)
            | CFn { .. }
            | PrintLit { .. } => {}
        }
        Ok(true)
    }
}

fn pattern_bindings(p: &TPat) -> Vec<u32> {
    match p {
        TPat::Bind(id) => vec![*id],
        TPat::Ctor { fields, .. } => fields
            .iter()
            .flat_map(|f| pattern_bindings(&f.pat))
            .collect(),
        TPat::Product { fields } => fields
            .iter()
            .flat_map(|f| pattern_bindings(&f.pat))
            .collect(),
        _ => vec![],
    }
}
