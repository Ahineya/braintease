//! Rust-only function tokens and typed closed-world dispatch. Tokens are
//! four-octet values, not C16 code pointers. Direct RCC calls preserve both
//! backends' existing resumable guest calling convention.
use super::*;
use rustc_span::Spanned;

#[derive(Clone, PartialEq, Eq, Hash)]
pub(super) struct Signature<'tcx> {
    inputs: Vec<Ty<'tcx>>,
    output: Ty<'tcx>,
    erased_receiver: bool,
}

impl<'tcx> Signature<'tcx> {
    fn new(
        tcx: TyCtxt<'tcx>,
        inputs: &[Ty<'tcx>],
        output: Ty<'tcx>,
        spread: bool,
        erased_receiver: bool,
    ) -> Result<Self> {
        let mut types = inputs.to_vec();
        if spread {
            let tuple = types.pop().ok_or("missing RustCall tuple")?;
            let ty::Tuple(fields) = tuple.kind() else {
                return Err("RustCall argument is not a tuple".into());
            };
            types.extend(fields.iter());
        }
        if erased_receiver {
            let first = types.first_mut().ok_or("missing dynamic receiver")?;
            if !matches!(first.kind(), ty::RawPtr(..) | ty::Ref(..)) {
                return Err(
                    "only reference/raw-pointer dynamic receivers are supported yet".into(),
                );
            }
            *first = Ty::new_mut_ptr(tcx, tcx.types.u8);
        }
        let normalize = |ty| {
            tcx.normalize_erasing_regions(
                TypingEnv::fully_monomorphized(),
                ty::Unnormalized::new_wip(ty),
            )
        };
        let mut inputs = Vec::new();
        for ty in types {
            let ty = normalize(ty);
            if layout::size(tcx, ty)? != 0 {
                inputs.push(ty);
            }
        }
        Ok(Self {
            inputs,
            output: normalize(output),
            erased_receiver,
        })
    }
}

impl<'tcx> Backend<'tcx> {
    pub(super) fn function_token(&mut self, instance: Instance<'tcx>) -> Result<u32> {
        if self.tcx.is_foreign_item(instance.def_id()) {
            return Err("foreign function pointers require an explicit bridge".into());
        }
        if matches!(
            instance.def,
            ty::InstanceKind::Virtual(..) | ty::InstanceKind::Intrinsic(..)
        ) {
            return Err("this function-pointer target requires a Rust wrapper".into());
        }
        if let Some(index) = self.function_tokens.iter().position(|i| *i == instance) {
            return Ok(index as u32 + 1);
        }
        let abi = if self.tcx.is_closure_like(instance.def_id()) {
            rustc_abi::ExternAbi::RustCall
        } else {
            self.tcx
                .fn_sig(instance.def_id())
                .instantiate(self.tcx, instance.args)
                .skip_binder()
                .abi()
        };
        if !matches!(
            abi,
            rustc_abi::ExternAbi::Rust | rustc_abi::ExternAbi::RustCall
        ) {
            return Err("only Rust function-pointer ABIs are supported; C16 callbacks need an explicit bridge".into());
        }
        self.function_tokens.push(instance);
        Ok(self.function_tokens.len() as u32)
    }

    fn body_signature(&self, instance: Instance<'tcx>, erased: bool) -> Result<Signature<'tcx>> {
        let body = self.tcx.instance_mir(instance.def);
        let mono = |ty| {
            instance.instantiate_mir_and_normalize_erasing_regions(
                self.tcx,
                TypingEnv::fully_monomorphized(),
                ty::EarlyBinder::bind(ty),
            )
        };
        let inputs: Vec<_> = (1..=body.arg_count)
            .map(|i| mono(body.local_decls[mir::Local::from_usize(i)].ty))
            .collect();
        if erased
            && !inputs.first().is_some_and(|t| {
                matches!(t.kind(), ty::RawPtr(..) | ty::Ref(..))
                    && layout::size(self.tcx, *t).is_ok_and(|n| n == 4)
            })
        {
            return Err("dynamic target requires a thin receiver".into());
        }
        Signature::new(
            self.tcx,
            &inputs,
            mono(body.return_ty()),
            body.spread_arg.is_some(),
            erased,
        )
    }

    fn dispatcher(&mut self, signature: Signature<'tcx>) -> String {
        let index = self
            .dispatches
            .iter()
            .position(|s| *s == signature)
            .unwrap_or_else(|| {
                self.dispatches.push(signature);
                self.dispatches.len() - 1
            });
        format!("rustcell_dispatch_{index}")
    }

    pub(super) fn dispatch_targets(&mut self) -> Result<bool> {
        let mut added = false;
        for signature in self.dispatches.clone() {
            for instance in self.function_tokens.clone() {
                if self
                    .body_signature(instance, signature.erased_receiver)
                    .is_ok_and(|s| s == signature)
                    && !self.names.contains_key(&instance)
                {
                    self.name(instance, self.queue[0])?;
                    added = true;
                }
            }
        }
        Ok(added)
    }

    pub(super) fn emit_dispatchers(&mut self) -> Result<()> {
        for (index, signature) in self.dispatches.clone().iter().enumerate() {
            let mut b = IrBuilder::new();
            let result = layout::carrier(self.tcx, signature.output)?;
            let returns = layout::size(self.tcx, signature.output)? != 0;
            let result_type = if returns {
                result.clone().unwrap_or(T::Void)
            } else {
                T::Void
            };
            b.create_function(format!("rustcell_dispatch_{index}"), result_type.clone());
            let mut args = Vec::new();
            if returns && result.is_none() {
                let id = b.new_temp();
                b.add_parameter(id, layout::pointer_type());
                args.push(mem::pointer(Value::Temp(id)));
            }
            let id = b.new_temp();
            b.add_parameter(id, T::I32);
            let token = Value::Temp(id);
            for ty in &signature.inputs {
                if layout::size(self.tcx, *ty)? == 0 {
                    continue;
                }
                let id = b.new_temp();
                let carrier = layout::carrier(self.tcx, *ty)?.unwrap_or_else(layout::pointer_type);
                let pointer = matches!(carrier, T::FatPtr(_));
                b.add_parameter(id, carrier);
                args.push(if pointer {
                    mem::pointer(Value::Temp(id))
                } else {
                    Value::Temp(id)
                });
            }
            let entry = b.new_label();
            b.create_block(entry)?;
            let mut targets = 0;
            for (index, instance) in self.function_tokens.iter().enumerate() {
                if !self
                    .body_signature(*instance, signature.erased_receiver)
                    .is_ok_and(|s| s == *signature)
                {
                    continue;
                }
                targets += 1;
                let yes = b.new_label();
                let no = b.new_label();
                let equal = mem::binary(
                    &mut b,
                    B::Eq,
                    token.clone(),
                    Value::Constant(index as i64 + 1),
                    T::I16,
                )?;
                b.build_branch_cond(equal, yes, no)?;
                b.create_block(yes)?;
                let value = b.build_call(
                    Value::Function(self.names[instance].clone()),
                    args.clone(),
                    result_type.clone(),
                )?;
                b.build_return(value.map(Value::Temp))?;
                b.create_block(no)?;
            }
            if targets == 0 {
                return Err(format!(
                    "no supported function target for dispatcher {index}"
                ));
            }
            b.build_call(Value::Function("rustcell_panic".into()), vec![], T::Void)?;
            let dead = b.new_label();
            b.build_branch(dead)?;
            b.create_block(dead)?;
            b.build_branch(dead)?;
            self.module
                .add_function(b.finish_function().ok_or("missing dispatcher")?);
        }
        Ok(())
    }
}

impl<'a, 'tcx> FunctionCx<'a, 'tcx> {
    pub(super) fn unsize_metadata(
        &mut self,
        source: Ty<'tcx>,
        target: Ty<'tcx>,
        old: Option<Value>,
    ) -> Result<Value> {
        let tcx = self.tcx();
        match (source.kind(), target.kind()) {
            (ty::Adt(a, x), ty::Adt(b, y)) if a == b && a.is_struct() => {
                if layout::dynamic_tail(tcx, target)
                    .is_some_and(|t| matches!(t.kind(), ty::Dynamic(..)))
                {
                    return Err(
                        "struct tails containing trait objects are not supported yet".into(),
                    );
                }
                let field = a
                    .non_enum_variant()
                    .fields
                    .iter()
                    .last()
                    .ok_or("empty unsizing struct")?;
                self.unsize_metadata(field.ty(tcx, x), field.ty(tcx, y), old)
            }
            (ty::Dynamic(a, _), ty::Dynamic(b, _)) => {
                let wide = old.ok_or("missing old trait-object metadata")?;
                let address = mem::offset(&mut self.b, wide, Value::Constant(4))?;
                let bits = mem::load_bits(&mut self.b, address, 4, T::I32)?;
                if a.principal_def_id() == b.principal_def_id() || b.principal_def_id().is_none() {
                    return Ok(bits);
                }
                if let Some(slot) = tcx.supertrait_vtable_slot((source, target)) {
                    let table = mem::pointer(mem::unary(
                        &mut self.b,
                        U::IntToPtr,
                        bits,
                        layout::pointer_type(),
                    )?);
                    let address =
                        mem::offset(&mut self.b, table, Value::Constant(slot as i64 * 4))?;
                    mem::load_bits(&mut self.b, address, 4, T::I32)
                } else {
                    Ok(bits)
                }
            }
            (_, ty::Dynamic(predicates, _)) => {
                let principal = predicates
                    .principal()
                    .map(|p| tcx.instantiate_bound_regions_with_erased(p));
                let id = tcx.vtable_allocation((source, principal));
                let table = self.allocation(id, 0, None)?;
                mem::unary(&mut self.b, U::PtrToInt, table, T::I32)
            }
            _ => self.unsize_length(source, target),
        }
    }

    pub(super) fn drop_dynamic(&mut self, data: Value, table: Value) -> Result<()> {
        let tcx = self.tcx();
        let table = mem::pointer(mem::unary(
            &mut self.b,
            U::IntToPtr,
            table,
            layout::pointer_type(),
        )?);
        let token = mem::load_bits(&mut self.b, table, 4, T::I32)?;
        let active = mem::binary(
            &mut self.b,
            B::Ne,
            token.clone(),
            Value::Constant(0),
            T::I16,
        )?;
        let yes = self.b.new_label();
        let done = self.b.new_label();
        self.b.build_branch_cond(active, yes, done)?;
        self.b.create_block(yes)?;
        let receiver = Ty::new_mut_ptr(tcx, tcx.types.u8);
        let signature = Signature::new(tcx, &[receiver], tcx.types.unit, false, true)?;
        let dispatcher = self.backend.dispatcher(signature);
        let token = mem::binary(&mut self.b, B::Or, token, Value::Constant(0), T::I32)?;
        self.b
            .build_call(Value::Function(dispatcher), vec![token, data], T::Void)?;
        self.b.build_branch(done)?;
        self.b.create_block(done)?;
        Ok(())
    }
    pub(super) fn call_values(
        &mut self,
        args: &[Spanned<Operand<'tcx>>],
        rust_call: bool,
    ) -> Result<Vec<Datum<'tcx>>> {
        let mut values = Vec::new();
        for (index, argument) in args.iter().enumerate() {
            let v = self.operand(&argument.node)?;
            if rust_call && index + 1 == args.len() {
                let ty::Tuple(fields) = v.ty.kind() else {
                    return Err("RustCall argument is not a tuple".into());
                };
                let l = layout::layout(self.tcx(), v.ty)?;
                for (field, ty) in fields.iter().enumerate() {
                    let p = mem::offset(
                        &mut self.b,
                        v.value.clone(),
                        Value::Constant(l.fields.offset(field).bytes() as i64),
                    )?;
                    let indirect = layout::carrier(self.tcx(), ty)?.is_none();
                    let value = if indirect || layout::size(self.tcx(), ty)? == 0 {
                        p
                    } else {
                        mem::load(&mut self.b, self.backend.tcx, p, ty)?
                    };
                    values.push(Datum {
                        value,
                        ty,
                        indirect,
                    });
                }
            } else {
                values.push(v);
            }
        }
        Ok(values)
    }

    pub(super) fn emit_call(
        &mut self,
        function: Value,
        values: Vec<Datum<'tcx>>,
        p: Address<'tcx>,
        token: Option<Value>,
    ) -> Result<()> {
        let tcx = self.tcx();
        let result_ty = layout::carrier(tcx, p.ty)?;
        let returns = layout::size(tcx, p.ty)? != 0;
        let mut arguments = Vec::new();
        if returns && result_ty.is_none() {
            arguments.push(p.pointer.clone());
        }
        if let Some(token) = token {
            arguments.push(mem::binary(
                &mut self.b,
                B::Or,
                token,
                Value::Constant(0),
                T::I32,
            )?);
        }
        for v in values {
            if layout::size(tcx, v.ty)? == 0 {
                continue;
            }
            if v.indirect {
                let l = layout::layout(tcx, v.ty)?;
                let copy = mem::alloca(&mut self.b, l.size.bytes(), l.align.abi.bytes())?;
                mem::copy(&mut self.b, copy.clone(), v.value, l.size.bytes())?;
                arguments.push(copy);
            } else {
                let carrier = layout::carrier(tcx, v.ty)?.unwrap();
                let value = if matches!(v.value, Value::Constant(_))
                    && matches!(carrier, T::I32 | T::I64)
                {
                    mem::binary(&mut self.b, B::Or, v.value, Value::Constant(0), carrier)?
                } else {
                    v.value
                };
                arguments.push(value);
            }
        }
        let result = self.b.build_call(
            function,
            arguments,
            if returns {
                result_ty.unwrap_or(T::Void)
            } else {
                T::Void
            },
        )?;
        if let Some(value) = result {
            mem::store(&mut self.b, tcx, p.pointer, Value::Temp(value), p.ty)?;
        }
        Ok(())
    }

    pub(super) fn dynamic_call(
        &mut self,
        func: &Operand<'tcx>,
        args: &[Spanned<Operand<'tcx>>],
        destination: Place<'tcx>,
        slot: Option<usize>,
    ) -> Result<()> {
        let tcx = self.tcx();
        let ty = self.mono(func.ty(&self.body.local_decls, tcx));
        let sig = tcx.instantiate_bound_regions_with_erased(ty.fn_sig(tcx));
        if !matches!(
            sig.abi(),
            rustc_abi::ExternAbi::Rust | rustc_abi::ExternAbi::RustCall
        ) {
            return Err("only Rust function-pointer ABIs are supported".into());
        }
        let spread = sig.abi() == rustc_abi::ExternAbi::RustCall;
        let signature = Signature::new(tcx, sig.inputs(), sig.output(), spread, slot.is_some())?;
        let mut values = self.call_values(args, spread)?;
        let token = if let Some(slot) = slot {
            let first = values.first_mut().ok_or("missing virtual receiver")?;
            if !first.indirect || layout::size(tcx, first.ty)? != 8 {
                return Err("unsupported virtual receiver representation".into());
            }
            let meta = mem::offset(&mut self.b, first.value.clone(), Value::Constant(4))?;
            let bits = mem::load_bits(&mut self.b, meta, 4, T::I32)?;
            let vtable = mem::pointer(mem::unary(
                &mut self.b,
                U::IntToPtr,
                bits,
                layout::pointer_type(),
            )?);
            let address = mem::offset(&mut self.b, vtable, Value::Constant(slot as i64 * 4))?;
            let token = mem::load_bits(&mut self.b, address, 4, T::I32)?;
            let bits = mem::load_bits(&mut self.b, first.value.clone(), 4, T::I32)?;
            *first = Datum {
                value: mem::pointer(mem::unary(
                    &mut self.b,
                    U::IntToPtr,
                    bits,
                    layout::pointer_type(),
                )?),
                ty: Ty::new_mut_ptr(tcx, tcx.types.u8),
                indirect: false,
            };
            token
        } else {
            self.scalar(func)?.value
        };
        let dispatcher = self.backend.dispatcher(signature);
        let p = self.address(destination)?;
        self.emit_call(Value::Function(dispatcher), values, p, Some(token))
    }
}
