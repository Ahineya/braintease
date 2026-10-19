//! Explicit first-order partial application. Slots are positional and direct;
//! inference never invents a type function by reversing an applied value type.
use super::*;

impl Checker<'_> {
    pub(in crate::check) fn partial_constructor(
        &mut self,
        base: Ty,
        slots: Vec<Option<Ty>>,
        span: usize,
    ) -> Result<Ty, CompileError> {
        self.check_kind(base, slots.len(), span)?;
        let (base, outer) = self.constructor_parts(base).unwrap();
        let mut supplied = slots.into_iter();
        let slots: Vec<_> = outer
            .into_iter()
            .map(|slot| match slot {
                Some(ty) => Some(ty),
                None => supplied.next().unwrap(),
            })
            .collect();
        let arity = slots.iter().filter(|slot| slot.is_none()).count();
        if arity == 0 {
            return Err(CompileError::at(
                self.source,
                span,
                "a partial constructor needs at least one `_` slot",
            ));
        }
        // Fixed arguments must be valid even when no value is ever constructed.
        if let Ty::Constructor(id) = base {
            if let Target::Adt(schema) = self.constructors[id as usize].target {
                let params = self.schemas[schema].type_params.clone();
                for (param, slot) in params.iter().zip(&slots) {
                    if let Some(ty) = slot {
                        self.check_bounds(std::slice::from_ref(param), &[*ty], span)?;
                    }
                }
            }
        }
        if slots.iter().all(Option::is_none) {
            return Ok(base);
        }
        let target = Target::Partial {
            base,
            slots: slots.clone(),
        };
        if let Some(id) = self.constructors.iter().position(|c| c.target == target) {
            return Ok(Ty::Constructor(id as u32));
        }
        let arguments = slots
            .iter()
            .map(|slot| slot.map(|ty| self.show(ty)).unwrap_or_else(|| "_".into()))
            .collect::<Vec<_>>()
            .join(", ");
        let name = format!("{}<{arguments}>", self.show(base));
        Ok(self.intern_constructor(name, arity, target))
    }

    pub(in crate::check) fn constructor_parts(
        &self,
        constructor: Ty,
    ) -> Option<(Ty, Vec<Option<Ty>>)> {
        let Ty::Constructor(id) = constructor else {
            return None;
        };
        let info = &self.constructors[id as usize];
        Some(match &info.target {
            Target::Partial { base, slots } => (*base, slots.clone()),
            _ => (constructor, vec![None; info.arity]),
        })
    }

    /// Project an application through a *known* constructor, checking its fixed
    /// arguments. Unknown F may only be inferred as a complete constructor.
    pub(in crate::check) fn project_constructor(
        &self,
        constructor: Ty,
        actual: Ty,
    ) -> Option<Vec<Ty>> {
        let (base, slots) = self.constructor_parts(constructor)?;
        let (found, args) = self.decompose_constructor(actual)?;
        if found != base || args.len() != slots.len() {
            return None;
        }
        let mut open = Vec::new();
        for (slot, arg) in slots.into_iter().zip(args) {
            match slot {
                Some(fixed) if fixed != arg => return None,
                Some(_) => {}
                None => open.push(arg),
            }
        }
        Some(open)
    }

    pub(in crate::check) fn infer_constructor_pattern(
        &self,
        parsed: &ParsedTy,
        actual: Ty,
        params: &[String],
        solved: &mut [Option<Ty>],
        span: usize,
    ) -> Result<(), CompileError> {
        let ParsedTy::Named { name, args, .. } = parsed else {
            unreachable!()
        };
        let (base, slots) = self.constructor_parts(actual).unwrap();
        let matches = self.constructor_names.get(name) == Some(&base)
            && (args.is_empty() && slots.iter().all(Option::is_none)
                || args.len() == slots.len()
                    && args
                        .iter()
                        .zip(&slots)
                        .all(|(p, a)| is_slot(p) == a.is_none()));
        if !matches {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "expected constructor {}, found {}",
                    parsed_brief(parsed),
                    self.show(actual)
                ),
            ));
        }
        for (arg, slot) in args.iter().zip(slots) {
            if let Some(ty) = slot {
                self.infer_field(arg, ty, params, solved, span)?;
            }
        }
        Ok(())
    }

    /// The head F is already known. Match F<_, E> structurally against actual
    /// constructor metadata, so E can be inferred inside constructor-bearing ADTs.
    pub(in crate::check) fn infer_partial_pattern(
        &self,
        constructor: Ty,
        patterns: &[ParsedTy],
        actual: Ty,
        params: &[String],
        solved: &mut [Option<Ty>],
        span: usize,
    ) -> Result<(), CompileError> {
        let (base, fixed) = self.constructor_parts(constructor).unwrap();
        let (found, slots) = self.constructor_parts(actual).unwrap();
        let mismatch = || {
            CompileError::at(
                self.source,
                span,
                format!(
                    "partial constructor pattern for {} does not match {}",
                    self.show(constructor),
                    self.show(actual)
                ),
            )
        };
        if base != found
            || fixed.len() != slots.len()
            || patterns.len() != fixed.iter().filter(|t| t.is_none()).count()
        {
            return Err(mismatch());
        }
        let mut patterns = patterns.iter();
        for (fixed, slot) in fixed.into_iter().zip(slots) {
            if let Some(ty) = fixed {
                if slot != Some(ty) {
                    return Err(mismatch());
                }
            } else {
                let pattern = patterns.next().unwrap();
                match slot {
                    None if is_slot(pattern) => {}
                    Some(ty) if !is_slot(pattern) => {
                        self.infer_field(pattern, ty, params, solved, span)?
                    }
                    _ => return Err(mismatch()),
                }
            }
        }
        Ok(())
    }

    pub(in crate::check) fn constructor_to_parsed(&self, constructor: Ty) -> ParsedTy {
        let (base, slots) = self.constructor_parts(constructor).unwrap();
        let Ty::Constructor(id) = base else {
            unreachable!()
        };
        let args = if base == constructor {
            Vec::new()
        } else {
            slots
                .into_iter()
                .map(|slot| {
                    slot.map(|ty| self.ty_to_parsed(ty))
                        .unwrap_or_else(|| ParsedTy::Named {
                            name: "_".into(),
                            args: Vec::new(),
                            span: Span::empty(),
                        })
                })
                .collect()
        };
        ParsedTy::Named {
            name: self.constructors[id as usize].name.clone(),
            args,
            span: Span::empty(),
        }
    }
}
