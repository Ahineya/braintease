//! Products in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn declare_structs(
        &mut self,
        items: &[crate::ast::StructItem],
    ) -> Result<(), CompileError> {
        for item in items {
            if self.struct_names.contains_key(&item.name)
                || Ty::parse_name(&item.name).is_some()
                || item.name == "Vec"
                || item.name == "Slice"
            {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    format!("duplicate type `{}`", item.name),
                ));
            }
            let id = self.structs.len() as u32;
            self.struct_names.insert(item.name.clone(), id);
            if item.name_span.end > item.name_span.start {
                self.type_defs.insert(item.name.clone(), item.name_span);
            }
            self.structs.push(StructInfo {
                name: item.name.clone(),
                fields: Vec::new(),
                width: 0,
                repr_c: item.repr_c,
            });
        }
        Ok(())
    }

    pub(super) fn finish_structs(
        &mut self,
        items: &[crate::ast::StructItem],
    ) -> Result<(), CompileError> {
        for item in items {
            let id = self.struct_names[&item.name];
            let mut fields = Vec::new();
            let mut seen = HashMap::new();
            for field in &item.fields {
                if seen.contains_key(&field.name) {
                    return Err(CompileError::at(
                        self.source,
                        field.name_span.start,
                        format!("duplicate field `{}`", field.name),
                    ));
                }
                seen.insert(field.name.clone(), ());
                if escaped_ref(&field.ty) {
                    return Err(CompileError::at(
                        self.source,
                        field.ty.span().start,
                        "a reference cannot be stored in a type",
                    ));
                }
                self.validate_ty(&field.ty, &[])?;
                fields.push((field.name.clone(), self.apply(&field.ty, &HashMap::new())?));
            }
            self.structs[id as usize].fields = fields;
        }
        let mut widths = vec![0u16; self.structs.len()];
        for (id, width) in widths.iter_mut().enumerate() {
            let mut stack = Vec::new();
            *width = self.measure_struct(id, &mut stack)?;
        }
        let mut tuple_w = vec![0u16; self.tuples.len()];
        for id in 0..self.tuples.len() {
            let mut sum = 0i64;
            for field in self.tuples[id].clone() {
                sum += self.raw_cells(field, &widths, &tuple_w);
            }
            if sum > u16::MAX as i64 {
                return Err(CompileError::at(self.source, 0, "tuple is too large"));
            }
            tuple_w[id] = sum as u16;
        }
        let mut struct_fields = Vec::new();
        for info in &self.structs {
            let mut fields = Vec::new();
            for (name, ty) in &info.fields {
                fields.push((
                    name.clone(),
                    rewrite_layout(*ty, &widths, &tuple_w, &self.arrays),
                ));
            }
            struct_fields.push(fields);
        }
        for (info, fields) in self.structs.iter_mut().zip(struct_fields) {
            info.fields = fields;
        }
        for (id, info) in self.structs.iter_mut().enumerate() {
            info.width = widths[id];
        }
        let mut tuple_fields = Vec::new();
        for fields in &self.tuples {
            tuple_fields.push(
                fields
                    .iter()
                    .map(|ty| rewrite_layout(*ty, &widths, &tuple_w, &self.arrays))
                    .collect::<Vec<_>>(),
            );
        }
        self.tuples = tuple_fields;
        for elems in [&mut self.vec_elems, &mut self.slice_elems] {
            for elem in elems {
                *elem = rewrite_layout(*elem, &widths, &tuple_w, &self.arrays);
            }
        }
        let elems: Vec<Ty> = self
            .arrays
            .iter()
            .map(|info| rewrite_layout(info.elem, &widths, &tuple_w, &self.arrays))
            .collect();
        for (info, elem) in self.arrays.iter_mut().zip(elems) {
            info.elem = elem;
        }
        self.check_repr(items)
    }

    pub(super) fn check_repr(&self, items: &[crate::ast::StructItem]) -> Result<(), CompileError> {
        for item in items {
            if !item.repr_c {
                continue;
            }
            let id = self.struct_names[&item.name];
            let info = &self.structs[id as usize];
            for (field, (_, ty)) in item.fields.iter().zip(&info.fields) {
                if self.c_layout_field(*ty) {
                    continue;
                }
                return Err(CompileError::at(
                    self.source,
                    field.ty.span().start,
                    format!(
                        "`{}` cannot be stored in a `#[repr(C)]` struct",
                        self.show(*ty)
                    ),
                ));
            }
        }
        Ok(())
    }

    pub(super) fn c_layout_field(&self, ty: Ty) -> bool {
        if ty.is_int() || ty.is_float() || self.is_repr_struct(ty) {
            return true;
        }
        if let Ty::Ptr(id) = ty {
            return self.c_pointee(self.ptrs[id as usize].inner);
        }
        false
    }

    pub(super) fn measure_struct(
        &self,
        id: usize,
        stack: &mut Vec<usize>,
    ) -> Result<u16, CompileError> {
        if stack.contains(&id) {
            return Err(CompileError::at(
                self.source,
                0,
                format!("struct `{}` contains itself", self.structs[id].name),
            ));
        }
        stack.push(id);
        let mut sum = 0i64;
        for (_, ty) in self.structs[id].fields.clone() {
            sum += self.measure_ty(ty, stack)?;
        }
        stack.pop();
        if sum > u16::MAX as i64 {
            return Err(CompileError::at(
                self.source,
                0,
                format!("struct `{}` is too large", self.structs[id].name),
            ));
        }
        Ok(sum as u16)
    }

    pub(super) fn measure_ty(&self, ty: Ty, stack: &mut Vec<usize>) -> Result<i64, CompileError> {
        match ty {
            Ty::Struct { id, .. } => Ok(self.measure_struct(id as usize, stack)? as i64),
            Ty::Tuple { id, .. } => {
                let mut sum = 0i64;
                for field in self.tuples[id as usize].clone() {
                    sum += self.measure_ty(field, stack)?;
                }
                Ok(sum)
            }
            Ty::Array { id, len, .. } => {
                Ok(len as i64 * self.measure_ty(self.arrays[id as usize].elem, stack)?)
            }
            other => Ok(other.cells()),
        }
    }

    pub(super) fn raw_cells(&self, ty: Ty, struct_w: &[u16], tuple_w: &[u16]) -> i64 {
        match ty {
            Ty::Struct { id, .. } => struct_w[id as usize] as i64,
            Ty::Tuple { id, .. } => tuple_w[id as usize] as i64,
            Ty::Array { id, len, .. } => {
                len as i64 * self.raw_cells(self.arrays[id as usize].elem, struct_w, tuple_w)
            }
            other => other.cells(),
        }
    }

    pub(super) fn struct_width(&self, id: u32) -> u16 {
        self.structs[id as usize].width
    }

    pub(super) fn intern_tuple(
        &mut self,
        span: usize,
        fields: Vec<Ty>,
    ) -> Result<Ty, CompileError> {
        if let Some(index) = self.tuples.iter().position(|got| got == &fields) {
            let width: i64 = self.tuples[index].iter().map(|ty| ty.cells()).sum();
            return Ok(Ty::Tuple {
                id: index as u32,
                width: width as u16,
            });
        }
        let width: i64 = fields.iter().map(|ty| ty.cells()).sum();
        if width > u16::MAX as i64 {
            return Err(CompileError::at(self.source, span, "tuple is too large"));
        }
        let id = self.tuples.len() as u32;
        self.tuples.push(fields);
        Ok(Ty::Tuple {
            id,
            width: width as u16,
        })
    }

    pub(super) fn check_tuple(
        &mut self,
        span: usize,
        fields: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let expected_fields = match expected {
            Some(Ty::Tuple { id, .. }) => Some(self.tuples[id as usize].clone()),
            Some(want) => {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found a tuple", self.show(want)),
                ));
            }
            None => None,
        };
        if let Some(want) = &expected_fields {
            if want.len() != fields.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {} fields, found {}", want.len(), fields.len()),
                ));
            }
        }
        let mut typed = Vec::new();
        for (index, field) in fields.iter().enumerate() {
            let want = expected_fields.as_ref().map(|fields| fields[index]);
            let value = self.check_expr(field, want)?;
            if let Some(want) = want {
                if value.ty != want {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "expected {}, found {}",
                            self.show(want),
                            self.show(value.ty)
                        ),
                    ));
                }
            }
            typed.push(value);
        }
        let tys: Vec<Ty> = typed.iter().map(|expr| expr.ty).collect();
        let ty = self.intern_tuple(span, tys)?;
        Ok(TExpr {
            ty,
            kind: TExprKind::Record { fields: typed },
        })
    }

    pub(super) fn check_struct_lit(
        &mut self,
        span: usize,
        name: &str,
        fields: &[(String, Expr)],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let Some(id) = self.struct_names.get(name).copied() else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown struct `{name}`"),
            ));
        };
        let info = self.structs[id as usize].fields.clone();
        let width = self.structs[id as usize].width;
        if fields.len() != info.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{name}` expects {} fields, found {}",
                    info.len(),
                    fields.len()
                ),
            ));
        }
        let mut seen = HashMap::new();
        for (field, _) in fields {
            if seen.contains_key(field) {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("duplicate field `{field}`"),
                ));
            }
            seen.insert(field.clone(), ());
        }
        let mut ordered = Vec::new();
        for (field_name, field_ty) in &info {
            let Some((_, expr)) = fields.iter().find(|(found, _)| found == field_name) else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("missing field `{field_name}`"),
                ));
            };
            let value = self.check_expr(expr, Some(*field_ty))?;
            if value.ty != *field_ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, found {}",
                        self.show(*field_ty),
                        self.show(value.ty)
                    ),
                ));
            }
            ordered.push(value);
        }
        let ty = Ty::Struct { id, width };
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {name}", self.show(want)),
                ));
            }
        }
        Ok(TExpr {
            ty,
            kind: TExprKind::Record { fields: ordered },
        })
    }

    pub(super) fn check_field(
        &mut self,
        span: usize,
        base: &Expr,
        name: &str,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let base = self.check_expr(base, None)?;
        let (offset, ty) = self.field_at(span, self.referent(base.ty), name)?;
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {}", self.show(want), self.show(ty)),
                ));
            }
        }
        Ok(TExpr {
            ty,
            kind: TExprKind::Field {
                base: Box::new(base),
                offset,
            },
        })
    }

    pub(super) fn field_at(
        &self,
        span: usize,
        ty: Ty,
        name: &str,
    ) -> Result<(i64, Ty), CompileError> {
        let Ty::Struct { id, .. } = ty else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("{} has no field `{name}`", self.show(ty)),
            ));
        };
        let mut offset = 0i64;
        for (field, field_ty) in &self.structs[id as usize].fields {
            if field == name {
                return Ok((offset, *field_ty));
            }
            offset += field_ty.cells();
        }
        Err(CompileError::at(
            self.source,
            span,
            format!("{} has no field `{name}`", self.show(ty)),
        ))
    }

    pub(super) fn check_field_index(
        &mut self,
        name: &str,
        span: usize,
        fields: &[String],
        index: &Expr,
        value: &Expr,
    ) -> Result<TStmt, CompileError> {
        let Some(id) = self.resolve(name) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown name `{name}`"),
            ));
        };
        let root = self.bindings[id as usize].ty;
        if !self.place_mutable(id, root) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` is not mutable"),
            ));
        }
        self.note_use(span, id, true)?;
        let mut ty = root;
        let mut offset = 0i64;
        for field in fields {
            let (at, next) = self.field_at(span, self.referent(ty), field)?;
            offset += at;
            ty = next;
        }
        if let Ty::Ptr(_) = ty {
            let ptr = TExpr {
                ty,
                kind: TExprKind::Field {
                    base: Box::new(TExpr {
                        ty: root,
                        kind: TExprKind::Name(id),
                    }),
                    offset,
                },
            };
            return self.store_through_ptr(span, ptr, Some(index), value);
        }
        if matches!(ty, Ty::Slice(_)) || matches!(self.referent(ty), Ty::Slice(_)) {
            return Err(CompileError::at(self.source, span, "slices are read-only"));
        }
        let elem = if let Ty::Vec(vec_id) = self.referent(ty) {
            self.vec_elems[vec_id as usize]
        } else if let Ty::Array { id: array_id, .. } = ty {
            self.arrays[array_id as usize].elem
        } else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{}` is not a vector or array", fields.join(".")),
            ));
        };
        let index = self.check_expr(index, Some(Ty::Usize))?;
        if index.ty != Ty::Usize {
            return Err(CompileError::at(
                self.source,
                span,
                format!("expected usize, found {}", self.show(index.ty)),
            ));
        }
        let value = self.check_expr(value, Some(elem))?;
        if value.ty != elem {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "expected {}, found {}",
                    self.show(elem),
                    self.show(value.ty)
                ),
            ));
        }
        Ok(TStmt::IndexAssign {
            binding: id,
            offset,
            index,
            value,
            elem,
            container: ty,
        })
    }

    pub(super) fn check_field_assign(
        &mut self,
        name: &str,
        span: usize,
        fields: &[String],
        value: &Expr,
    ) -> Result<TStmt, CompileError> {
        let Some(id) = self.resolve(name) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown name `{name}`"),
            ));
        };
        let root = self.bindings[id as usize].ty;
        if !self.place_mutable(id, root) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` is not mutable"),
            ));
        }
        self.note_use(span, id, true)?;
        let mut ty = self.referent(root);
        let mut offset = 0i64;
        for field in fields {
            let (at, next) = self.field_at(span, ty, field)?;
            offset += at;
            ty = next;
        }
        let value = self.check_expr(value, Some(ty))?;
        if value.ty != ty {
            return Err(CompileError::at(
                self.source,
                span,
                format!("expected {}, found {}", self.show(ty), self.show(value.ty)),
            ));
        }
        Ok(TStmt::FieldAssign {
            binding: id,
            offset,
            value,
        })
    }
}
