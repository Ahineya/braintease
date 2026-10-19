//! Runtime operations with an explicit Rust-octet/C16-carrier boundary.
use super::*;
use rustc_span::Spanned;

impl<'a, 'tcx> FunctionCx<'a, 'tcx> {
    fn runtime_call(
        &mut self,
        name: &str,
        arguments: Vec<(Value, T)>,
        result: T,
    ) -> Result<Option<Value>> {
        let mut values = Vec::new();
        for (mut value, ty) in arguments {
            if matches!(value, Value::Constant(_)) && matches!(ty, T::I32 | T::I64) {
                value = mem::binary(&mut self.b, B::Or, value, Value::Constant(0), ty)?;
            }
            values.push(value);
        }
        Ok(self
            .b
            .build_call(Value::Function(name.into()), values, result)?
            .map(Value::Temp))
    }

    /// Only the pinned alloc crate's internal declarations receive this ABI.
    /// User extern declarations still pass through check_bridge.
    pub(super) fn allocator_call(
        &mut self,
        instance: Instance<'tcx>,
        args: &[Spanned<Operand<'tcx>>],
        destination: Place<'tcx>,
    ) -> Result<bool> {
        let tcx = self.tcx();
        if !tcx.is_foreign_item(instance.def_id())
            || tcx.crate_name(instance.def_id().krate).as_str() != "alloc"
        {
            return Ok(false);
        }
        let name = match tcx.item_name(instance.def_id()).as_str() {
            "__rust_alloc" => "rustcell_alloc",
            "__rust_alloc_zeroed" => "rustcell_alloc_zeroed",
            "__rust_dealloc" => "rustcell_dealloc",
            "__rust_realloc" => "rustcell_realloc",
            "__rust_no_alloc_shim_is_unstable_v2" => return Ok(true),
            _ => return Ok(false),
        };
        let mut values = Vec::new();
        for (index, argument) in args.iter().enumerate() {
            let d = self.operand(&argument.node)?;
            if layout::size(tcx, d.ty)? != 4 {
                return Err(format!("unexpected allocator argument: {}", d.ty));
            }
            let pointer = index == 0 && matches!(name, "rustcell_dealloc" | "rustcell_realloc");
            let raw = if d.indirect {
                mem::load_bits(&mut self.b, d.value, 4, T::I32)?
            } else {
                d.value
            };
            let value = if pointer {
                mem::pointer(mem::unary(
                    &mut self.b,
                    U::IntToPtr,
                    raw,
                    layout::pointer_type(),
                )?)
            } else {
                raw
            };
            values.push((
                value,
                if pointer {
                    layout::pointer_type()
                } else {
                    T::I32
                },
            ));
        }
        let p = self.address(destination)?;
        let result = if name == "rustcell_dealloc" {
            T::Void
        } else {
            layout::pointer_type()
        };
        if let Some(value) = self.runtime_call(name, values, result)? {
            mem::store(&mut self.b, tcx, p.pointer, value, p.ty)?;
        }
        Ok(true)
    }

    pub(super) fn copy_elements(
        &mut self,
        source: &Operand<'tcx>,
        destination: &Operand<'tcx>,
        count: &Operand<'tcx>,
    ) -> Result<()> {
        let source = self.scalar(source)?;
        let destination = self.scalar(destination)?;
        let count = self.scalar(count)?;
        let stride = layout::size(self.tcx(), layout::pointee(source.ty)?)?;
        let bytes = mem::binary(
            &mut self.b,
            B::Mul,
            count.value,
            Value::Constant(stride as i64),
            T::I32,
        )?;
        self.runtime_call(
            "rustcell_copy",
            vec![
                (destination.value, layout::pointer_type()),
                (source.value, layout::pointer_type()),
                (bytes, T::I32),
            ],
            T::Void,
        )?;
        Ok(())
    }

    pub(super) fn memory_intrinsic(
        &mut self,
        name: &str,
        instance: Instance<'tcx>,
        args: &[Spanned<Operand<'tcx>>],
        p: &Address<'tcx>,
    ) -> Result<bool> {
        let tcx = self.tcx();
        match name {
            "cttz" | "cttz_nonzero" | "ctlz" | "ctlz_nonzero" => {
                let arg = self.scalar(&args[0].node)?;
                let input = layout::carrier(tcx, arg.ty)?.ok_or("non-scalar zero count")?;
                let output = layout::carrier(tcx, p.ty)?.ok_or("non-scalar zero count result")?;
                let bits = layout::size(tcx, arg.ty)? * 8;
                let mut active = Value::Constant(1);
                let mut count = Value::Constant(0);
                for index in 0..bits {
                    let index = if name.starts_with("ctlz") {
                        bits - index - 1
                    } else {
                        index
                    };
                    let part = mem::binary(
                        &mut self.b,
                        B::LShr,
                        arg.value.clone(),
                        Value::Constant(index as i64),
                        input.clone(),
                    )?;
                    let part =
                        mem::binary(&mut self.b, B::And, part, Value::Constant(1), input.clone())?;
                    let zero = mem::binary(&mut self.b, B::Eq, part, Value::Constant(0), T::I16)?;
                    active = mem::binary(&mut self.b, B::And, active, zero, T::I16)?;
                    let term = if output == T::I16 {
                        active.clone()
                    } else {
                        mem::unary(&mut self.b, U::ZExt, active.clone(), output.clone())?
                    };
                    count = mem::binary(&mut self.b, B::Add, count, term, output.clone())?;
                }
                mem::store(&mut self.b, tcx, p.pointer.clone(), count, p.ty)?;
            }
            "exact_div" => {
                let left = self.scalar(&args[0].node)?;
                let right = self.scalar(&args[1].node)?;
                let register = layout::carrier(tcx, left.ty)?.ok_or("non-scalar exact division")?;
                let result = mem::binary(
                    &mut self.b,
                    if layout::signed(left.ty) {
                        B::SDiv
                    } else {
                        B::UDiv
                    },
                    left.value,
                    right.value,
                    register,
                )?;
                mem::store(&mut self.b, tcx, p.pointer.clone(), result, p.ty)?;
            }
            "compare_bytes" => {
                let left = self.scalar(&args[0].node)?;
                let right = self.scalar(&args[1].node)?;
                let count = self.scalar(&args[2].node)?;
                let result = self
                    .runtime_call(
                        "rustcell_compare",
                        vec![
                            (left.value, layout::pointer_type()),
                            (right.value, layout::pointer_type()),
                            (count.value, T::I32),
                        ],
                        T::I32,
                    )?
                    .ok_or("missing byte comparison result")?;
                mem::store(&mut self.b, tcx, p.pointer.clone(), result, p.ty)?;
            }
            "bswap" => {
                let arg = self.scalar(&args[0].node)?;
                let register = layout::carrier(tcx, arg.ty)?.ok_or("non-scalar byte swap")?;
                let bytes = layout::size(tcx, arg.ty)?;
                let mut result = Value::Constant(0);
                for index in 0..bytes {
                    let byte = mem::binary(
                        &mut self.b,
                        B::LShr,
                        arg.value.clone(),
                        Value::Constant((index * 8) as i64),
                        register.clone(),
                    )?;
                    let byte = mem::binary(
                        &mut self.b,
                        B::And,
                        byte,
                        Value::Constant(255),
                        register.clone(),
                    )?;
                    let byte = mem::binary(
                        &mut self.b,
                        B::Shl,
                        byte,
                        Value::Constant(((bytes - index - 1) * 8) as i64),
                        register.clone(),
                    )?;
                    result = mem::binary(&mut self.b, B::Or, result, byte, register.clone())?;
                }
                mem::store(&mut self.b, tcx, p.pointer.clone(), result, p.ty)?;
            }
            "ctpop" => {
                let arg = self.scalar(&args[0].node)?;
                let register =
                    layout::carrier(tcx, arg.ty)?.ok_or("non-scalar population count")?;
                let mut count = Value::Constant(0);
                for bit in 0..layout::size(tcx, arg.ty)? * 8 {
                    let part = mem::binary(
                        &mut self.b,
                        B::LShr,
                        arg.value.clone(),
                        Value::Constant(bit as i64),
                        register.clone(),
                    )?;
                    let part = mem::binary(
                        &mut self.b,
                        B::And,
                        part,
                        Value::Constant(1),
                        register.clone(),
                    )?;
                    let part = if register == T::I32 {
                        part
                    } else {
                        mem::unary(
                            &mut self.b,
                            if register == T::I16 {
                                U::ZExt
                            } else {
                                U::Trunc
                            },
                            part,
                            T::I32,
                        )?
                    };
                    count = mem::binary(&mut self.b, B::Add, count, part, T::I32)?;
                }
                mem::store(&mut self.b, tcx, p.pointer.clone(), count, p.ty)?;
            }
            "copy" | "copy_nonoverlapping" => {
                self.copy_elements(&args[0].node, &args[1].node, &args[2].node)?
            }
            "write_bytes" => {
                let dst = self.scalar(&args[0].node)?;
                let byte = self.scalar(&args[1].node)?;
                let count = self.scalar(&args[2].node)?;
                let stride = layout::size(tcx, instance.args.type_at(0))?;
                let bytes = mem::binary(
                    &mut self.b,
                    B::Mul,
                    count.value,
                    Value::Constant(stride as i64),
                    T::I32,
                )?;
                self.runtime_call(
                    "rustcell_fill",
                    vec![
                        (dst.value, layout::pointer_type()),
                        (byte.value, T::I16),
                        (bytes, T::I32),
                    ],
                    T::Void,
                )?;
            }
            "read_via_copy" => {
                let src = self.scalar(&args[0].node)?;
                mem::copy(
                    &mut self.b,
                    p.pointer.clone(),
                    src.value,
                    layout::size(tcx, p.ty)?,
                )?;
            }
            "write_via_move" => {
                let dst = self.scalar(&args[0].node)?;
                let value = self.operand(&args[1].node)?;
                self.assign(dst.value, value.ty, value)?;
            }
            "ptr_offset_from" | "ptr_offset_from_unsigned" => {
                let left = self.scalar(&args[0].node)?;
                let right = self.scalar(&args[1].node)?;
                let left = mem::unary(
                    &mut self.b,
                    U::PtrToLinear,
                    mem::pointer(left.value),
                    T::I32,
                )?;
                let right = mem::unary(
                    &mut self.b,
                    U::PtrToLinear,
                    mem::pointer(right.value),
                    T::I32,
                )?;
                let cells = mem::binary(&mut self.b, B::Sub, left, right, T::I32)?;
                let stride = layout::size(tcx, instance.args.type_at(0))?;
                if stride == 0 {
                    return Err("pointer difference on zero-sized elements".into());
                }
                let value = mem::binary(
                    &mut self.b,
                    if name.ends_with("unsigned") {
                        B::UDiv
                    } else {
                        B::SDiv
                    },
                    cells,
                    Value::Constant(stride as i64),
                    T::I32,
                )?;
                mem::store(&mut self.b, tcx, p.pointer.clone(), value, p.ty)?;
            }
            "size_of_val" | "align_of_val" => {
                let arg = self.operand(&args[0].node)?;
                let pointee = layout::pointee(arg.ty)?;
                if matches!(pointee.kind(), ty::Adt(..))
                    && layout::dynamic_tail(tcx, pointee)
                        .is_some_and(|t| matches!(t.kind(), ty::Dynamic(..)))
                {
                    return Err(
                        "struct tails containing trait objects are not supported yet".into(),
                    );
                }
                if matches!(pointee.kind(), ty::Dynamic(..)) {
                    let address = mem::offset(&mut self.b, arg.value, Value::Constant(4))?;
                    let bits = mem::load_bits(&mut self.b, address, 4, T::I32)?;
                    let table = mem::pointer(mem::unary(
                        &mut self.b,
                        U::IntToPtr,
                        bits,
                        layout::pointer_type(),
                    )?);
                    let address = mem::offset(
                        &mut self.b,
                        table,
                        Value::Constant(if name == "align_of_val" { 8 } else { 4 }),
                    )?;
                    let value = mem::load_bits(&mut self.b, address, 4, T::I32)?;
                    mem::store(&mut self.b, tcx, p.pointer.clone(), value, p.ty)?;
                    return Ok(true);
                }
                if matches!(pointee.kind(), ty::Adt(..))
                    && layout::slice_tail(tcx, pointee).is_some()
                {
                    let l = layout::layout(tcx, pointee)?;
                    let align = l.align.abi.bytes();
                    let value = if name == "align_of_val" {
                        Value::Constant(align as i64)
                    } else {
                        let (prefix, stride) = layout::slice_tail_layout(tcx, pointee)?;
                        let metadata = mem::offset(&mut self.b, arg.value, Value::Constant(4))?;
                        let length = mem::load_bits(&mut self.b, metadata, 4, T::I32)?;
                        let tail = mem::binary(
                            &mut self.b,
                            B::Mul,
                            length,
                            Value::Constant(stride as i64),
                            T::I32,
                        )?;
                        let bytes = mem::binary(
                            &mut self.b,
                            B::Add,
                            tail,
                            Value::Constant((prefix + align - 1) as i64),
                            T::I32,
                        )?;
                        mem::binary(
                            &mut self.b,
                            B::And,
                            bytes,
                            Value::Constant(-(align as i64)),
                            T::I32,
                        )?
                    };
                    mem::store(&mut self.b, tcx, p.pointer.clone(), value, p.ty)?;
                    return Ok(true);
                }
                let (element, metadata) = match pointee.kind() {
                    ty::Slice(t) => (Some(*t), true),
                    ty::Str => (Some(tcx.types.u8), true),
                    _ => (None, false),
                };
                let l = layout::layout(tcx, element.unwrap_or(pointee))?;
                let value = if name == "align_of_val" {
                    Value::Constant(l.align.abi.bytes() as i64)
                } else if metadata {
                    let length = mem::offset(&mut self.b, arg.value, Value::Constant(4))?;
                    let length = mem::load_bits(&mut self.b, length, 4, T::I32)?;
                    mem::binary(
                        &mut self.b,
                        B::Mul,
                        length,
                        Value::Constant(l.size.bytes() as i64),
                        T::I32,
                    )?
                } else {
                    Value::Constant(l.size.bytes() as i64)
                };
                mem::store(&mut self.b, tcx, p.pointer.clone(), value, p.ty)?;
            }
            _ => return Ok(false),
        }
        Ok(true)
    }
}
