//! Types in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn validate_ty(&self, ty: &ParsedTy, params: &[String]) -> Result<(), CompileError> {
        match ty {
            ParsedTy::Func {
                params: fn_params,
                ret,
                ..
            }
            | ParsedTy::CFn {
                params: fn_params,
                ret,
                ..
            } => {
                for param in fn_params {
                    self.validate_ty(param, params)?;
                }
                self.validate_ty(ret, params)
            }
            ParsedTy::Ref { inner, .. } => self.validate_ty(inner, params),
            ParsedTy::Ptr { inner, .. } => self.validate_ty(inner, params),
            ParsedTy::Array { elem, .. } => self.validate_ty(elem, params),
            ParsedTy::Tuple { fields, .. } => {
                for field in fields {
                    self.validate_ty(field, params)?;
                }
                Ok(())
            }
            ParsedTy::Named { span, name, args } => {
                if params.iter().any(|param| param == name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("type parameter `{name}` does not take arguments"),
                        ));
                    }
                    return Ok(());
                }
                if Ty::parse_name(name).is_some() {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    return Ok(());
                }
                if name == "Vec" || name == "Slice" {
                    if args.len() != 1 {
                        return Err(self.arity_error(span.start, name, 1, args.len()));
                    }
                    return self.validate_ty(&args[0], params);
                }
                if self.struct_names.contains_key(name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    return Ok(());
                }
                let Some(schema) = self.schemas.iter().find(|schema| schema.name == *name) else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("unknown type `{name}`"),
                    ));
                };
                if args.len() != schema.params.len() {
                    return Err(self.arity_error(
                        span.start,
                        name,
                        schema.params.len(),
                        args.len(),
                    ));
                }
                for arg in args {
                    self.validate_ty(arg, params)?;
                }
                Ok(())
            }
        }
    }

    pub(super) fn arity_error(
        &self,
        offset: usize,
        name: &str,
        expect: usize,
        found: usize,
    ) -> CompileError {
        if expect == 0 {
            CompileError::at(
                self.source,
                offset,
                format!("`{name}` does not take type arguments"),
            )
        } else {
            CompileError::at(
                self.source,
                offset,
                format!("`{name}` expects {expect} type arguments, found {found}"),
            )
        }
    }

    pub(super) fn monomorphize(&mut self, schema: usize, args: &[Ty]) -> Result<u32, CompileError> {
        let key = (schema, args.to_vec());
        if let Some(id) = self.mono.get(&key) {
            return Ok(*id);
        }
        let name = self.schema_name(schema);
        let params = self.schemas[schema].params.clone();
        let ctors = self.schemas[schema].ctors.clone();
        let id = self.adts.len() as u32;
        let display = applied_name(&name, args, self);
        self.adts.push(AdtDef {
            name: display,
            schema,
            args: args.to_vec(),
            ctors: Vec::new(),
        });
        self.mono.insert(key, id);
        let mut subst = HashMap::new();
        for (param, arg) in params.iter().zip(args) {
            subst.insert(param.clone(), *arg);
        }
        for ctor in ctors {
            let mut fields = Vec::new();
            for field in &ctor {
                fields.push(self.apply(field, &subst)?);
            }
            self.adts[id as usize].ctors.push(CtorDef { fields });
        }
        Ok(id)
    }

    pub(super) fn schema_name(&self, schema: usize) -> String {
        self.schemas[schema].name.clone()
    }

    pub(super) fn apply(
        &mut self,
        ty: &ParsedTy,
        subst: &HashMap<String, Ty>,
    ) -> Result<Ty, CompileError> {
        match ty {
            ParsedTy::Func { params, ret, .. } => {
                let mut fn_params = Vec::new();
                for param in params {
                    fn_params.push(self.apply(param, subst)?);
                }
                let fn_ret = self.apply(ret, subst)?;
                Ok(self.intern_func(fn_params, fn_ret))
            }
            ParsedTy::CFn { span, params, ret } => {
                let mut fn_params = Vec::new();
                for param in params {
                    let ty = self.apply(param, subst)?;
                    self.require_c_type(span.start, ty, false)?;
                    fn_params.push(ty);
                }
                let fn_ret = self.apply(ret, subst)?;
                self.require_c_type(span.start, fn_ret, true)?;
                Ok(self.intern_cfn(fn_params, fn_ret))
            }
            ParsedTy::Ref {
                span,
                mutable,
                inner,
            } => {
                let inner = self.apply(inner, subst)?;
                if inner.is_ref() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        "cannot borrow a reference",
                    ));
                }
                Ok(self.intern_ref(*mutable, inner))
            }
            ParsedTy::Ptr { mutable, inner, .. } => {
                let inner = self.apply(inner, subst)?;
                Ok(self.intern_ptr(*mutable, inner))
            }
            ParsedTy::Array { span, elem, len } => {
                let elem = self.apply(elem, subst)?;
                self.require_vec_elem(span.start, elem)?;
                let (_, value) = self.eval_const(len, Some(Ty::Usize))?;
                if !(0..=u16::MAX as i64).contains(&value) {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("array length {value} does not fit in usize"),
                    ));
                }
                Ok(self.intern_array(elem, value as u16))
            }
            ParsedTy::Tuple { span, fields } => {
                let mut typed = Vec::new();
                for field in fields {
                    typed.push(self.apply(field, subst)?);
                }
                self.intern_tuple(span.start, typed)
            }
            ParsedTy::Named { span, name, args } => {
                if let Some(found) = subst.get(name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("type parameter `{name}` does not take arguments"),
                        ));
                    }
                    return Ok(*found);
                }
                if let Some(prim) = Ty::parse_name(name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    return Ok(prim);
                }
                if name == "Vec" || name == "Slice" {
                    if args.len() != 1 {
                        return Err(self.arity_error(span.start, name, 1, args.len()));
                    }
                    let elem = self.apply(&args[0], subst)?;
                    self.require_vec_elem(span.start, elem)?;
                    return Ok(if name == "Vec" {
                        self.intern_vec(elem)
                    } else {
                        self.intern_slice(elem)
                    });
                }
                if let Some(id) = self.struct_names.get(name).copied() {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    self.note_type(*span, name);
                    return Ok(Ty::Struct {
                        id,
                        width: self.struct_width(id),
                    });
                }
                let Some(schema) = self.schemas.iter().position(|schema| schema.name == *name)
                else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("unknown type `{name}`"),
                    ));
                };
                self.note_type(*span, name);
                let expect = self.schemas[schema].params.len();
                if args.len() != expect {
                    return Err(self.arity_error(span.start, name, expect, args.len()));
                }
                let mut applied = Vec::new();
                for arg in args {
                    applied.push(self.apply(arg, subst)?);
                }
                let id = self.monomorphize(schema, &applied)?;
                Ok(Ty::Adt(id))
            }
        }
    }

    pub(super) fn intern_ptr(&mut self, mutable: bool, inner: Ty) -> Ty {
        if let Some(index) = self
            .ptrs
            .iter()
            .position(|ptr| ptr.mutable == mutable && ptr.inner == inner)
        {
            return Ty::Ptr(index as u32);
        }
        let id = self.ptrs.len() as u32;
        self.ptrs.push(RefTy { mutable, inner });
        Ty::Ptr(id)
    }

    pub(super) fn intern_ref(&mut self, mutable: bool, inner: Ty) -> Ty {
        if let Some(index) = self
            .refs
            .iter()
            .position(|refty| refty.mutable == mutable && refty.inner == inner)
        {
            return Ty::Ref(index as u32);
        }
        let id = self.refs.len() as u32;
        self.refs.push(RefTy { mutable, inner });
        Ty::Ref(id)
    }

    pub(super) fn referent(&self, ty: Ty) -> Ty {
        if let Ty::Ref(id) = ty {
            self.refs[id as usize].inner
        } else {
            ty
        }
    }

    pub(super) fn intern_array(&mut self, elem: Ty, len: u16) -> Ty {
        let width = elem.cells() as u16;
        if let Some(index) = self
            .arrays
            .iter()
            .position(|info| info.elem == elem && info.len == len)
        {
            return Ty::Array {
                id: index as u32,
                len,
                width,
            };
        }
        let id = self.arrays.len() as u32;
        self.arrays.push(ArrayInfo { elem, len });
        Ty::Array { id, len, width }
    }

    pub(super) fn intern_slice(&mut self, elem: Ty) -> Ty {
        if let Some(index) = self.slice_elems.iter().position(|ty| *ty == elem) {
            return Ty::Slice(index as u32);
        }
        let id = self.slice_elems.len() as u32;
        self.slice_elems.push(elem);
        Ty::Slice(id)
    }

    pub(super) fn intern_vec(&mut self, elem: Ty) -> Ty {
        if let Some(index) = self.vec_elems.iter().position(|ty| *ty == elem) {
            return Ty::Vec(index as u32);
        }
        let id = self.vec_elems.len() as u32;
        self.vec_elems.push(elem);
        Ty::Vec(id)
    }

    pub(super) fn require_vec_elem(&self, span: usize, elem: Ty) -> Result<(), CompileError> {
        if !self.collection_borrows(elem) {
            return Ok(());
        }
        Err(CompileError::at(
            self.source,
            span,
            "collection elements cannot contain borrowed references",
        ))
    }

    fn collection_borrows(&self, ty: Ty) -> bool {
        match ty {
            Ty::Ref(_) => true,
            Ty::Tuple { id, .. } => self.tuples[id as usize]
                .iter()
                .any(|t| self.collection_borrows(*t)),
            Ty::Array { id, .. } => self.collection_borrows(self.arrays[id as usize].elem),
            _ => false, // Named aggregates already reject stored references.
        }
    }

    pub(super) fn intern_cfn(&mut self, params: Vec<Ty>, ret: Ty) -> Ty {
        if let Some(index) = self
            .cfns
            .iter()
            .position(|sig| sig.params == params && sig.ret == ret)
        {
            return Ty::CFn(index as u32);
        }
        let id = self.cfns.len() as u32;
        self.cfns.push(FuncSig { params, ret });
        Ty::CFn(id)
    }

    pub(super) fn intern_func(&mut self, params: Vec<Ty>, ret: Ty) -> Ty {
        if let Some(index) = self
            .funcs
            .iter()
            .position(|sig| sig.params == params && sig.ret == ret)
        {
            return Ty::Func(index as u32);
        }
        let id = self.funcs.len() as u32;
        self.funcs.push(FuncSig { params, ret });
        Ty::Func(id)
    }
}
