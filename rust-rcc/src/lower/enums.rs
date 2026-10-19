//! Use rustc's selected enum layout, including niches. Never invent a tag or
//! place variant fields directly after it: both choices belong to rustc.
use super::*;
use rustc_abi::{Primitive, TagEncoding, VariantIdx, Variants};
use rustc_middle::ty::layout::{LayoutCx, TyAndLayout};

impl<'a, 'tcx> FunctionCx<'a, 'tcx> {
    pub(super) fn variant_layout(
        &self,
        ty: Ty<'tcx>,
        variant: Option<VariantIdx>,
    ) -> Result<TyAndLayout<'tcx>> {
        let l = layout::layout(self.tcx(), ty)?;
        Ok(match variant {
            Some(v) => l.for_variant(
                &LayoutCx::new(self.tcx(), TypingEnv::fully_monomorphized()),
                v,
            ),
            None => l,
        })
    }

    pub(super) fn set_discriminant(
        &mut self,
        pointer: Value,
        ty: Ty<'tcx>,
        variant: VariantIdx,
    ) -> Result<()> {
        let l = layout::layout(self.tcx(), ty)?;
        if self.variant_layout(ty, Some(variant))?.is_uninhabited() {
            return Err(format!(
                "cannot construct uninhabited variant {variant:?} of {ty}"
            ));
        }
        let (tag, field, value) = match &l.variants {
            Variants::Empty => return Err(format!("uninhabited type: {ty}")),
            Variants::Single { index } if *index == variant => return Ok(()),
            Variants::Single { .. } => return Err("incorrect single enum variant".into()),
            Variants::Multiple {
                tag,
                tag_field,
                tag_encoding,
                ..
            } => {
                let value = match tag_encoding {
                    TagEncoding::Direct => {
                        ty.discriminant_for_variant(self.tcx(), variant)
                            .ok_or("missing enum discriminant")?
                            .val
                    }
                    TagEncoding::Niche {
                        untagged_variant,
                        niche_variants,
                        niche_start,
                    } => {
                        if variant == *untagged_variant {
                            return Ok(());
                        }
                        if !niche_variants.contains(&variant) {
                            return Err("variant is outside its niche range".into());
                        }
                        niche_start.wrapping_add(u128::from(
                            variant.as_u32() - niche_variants.start().as_u32(),
                        ))
                    }
                };
                (*tag, *tag_field, value)
            }
        };
        let bytes = tag.size(&self.tcx()).bytes();
        let carrier = layout::integer_carrier(bytes)?;
        let address = mem::offset(
            &mut self.b,
            pointer,
            Value::Constant(l.fields.offset(field.index()).bytes() as i64),
        )?;
        // The storage operation masks off bits outside the tag width, including
        // sign-extended discriminants and niche arithmetic wrapping at 8 bits.
        mem::store_bits(
            &mut self.b,
            address,
            Value::Constant(value as i64),
            bytes,
            carrier,
        )
    }

    pub(super) fn read_discriminant(
        &mut self,
        pointer: Value,
        ty: Ty<'tcx>,
        result_ty: Ty<'tcx>,
    ) -> Result<Value> {
        let l = layout::layout(self.tcx(), ty)?;
        let (tag, field, encoding) = match &l.variants {
            Variants::Empty => return Err(format!("discriminant of uninhabited type: {ty}")),
            Variants::Single { index } => {
                return Ok(Value::Constant(
                    ty.discriminant_for_variant(self.tcx(), *index)
                        .map(|d| d.val as i64)
                        .unwrap_or(0),
                ));
            }
            Variants::Multiple {
                tag,
                tag_field,
                tag_encoding,
                ..
            } => (*tag, *tag_field, tag_encoding.clone()),
        };
        let bytes = tag.size(&self.tcx()).bytes();
        let register = layout::integer_carrier(bytes)?;
        let address = mem::offset(
            &mut self.b,
            pointer,
            Value::Constant(l.fields.offset(field.index()).bytes() as i64),
        )?;
        let value = mem::load_bits(&mut self.b, address, bytes, register.clone())?;
        let output_bytes = layout::size(self.tcx(), result_ty)?;
        let output_register = layout::integer_carrier(output_bytes)?;
        match encoding {
            TagEncoding::Direct => {
                let signed = matches!(tag.primitive(), Primitive::Int(_, true)) && !tag.is_bool();
                self.resize_tag(value, bytes, output_bytes, signed)
            }
            TagEncoding::Niche {
                untagged_variant,
                niche_variants,
                niche_start,
            } => {
                let mut relative = mem::binary(
                    &mut self.b,
                    B::Sub,
                    value,
                    Value::Constant(niche_start as i64),
                    register,
                )?;
                // Our I16 carrier must still wrap an eight-bit niche subtraction.
                if bytes == 1 {
                    relative =
                        mem::binary(&mut self.b, B::And, relative, Value::Constant(255), T::I16)?;
                }
                let maximum = niche_variants.end().as_u32() - niche_variants.start().as_u32();
                let is_niche = mem::binary(
                    &mut self.b,
                    B::Ule,
                    relative.clone(),
                    Value::Constant(i64::from(maximum)),
                    T::I16,
                )?;
                let slot = mem::alloca(&mut self.b, output_bytes, 1)?;
                let yes = self.b.new_label();
                let no = self.b.new_label();
                let done = self.b.new_label();
                self.b.build_branch_cond(is_niche, yes, no)?;
                self.b.create_block(yes)?;
                let relative = self.resize_tag(relative, bytes, output_bytes, false)?;
                let discriminant = mem::binary(
                    &mut self.b,
                    B::Add,
                    relative,
                    Value::Constant(i64::from(niche_variants.start().as_u32())),
                    output_register.clone(),
                )?;
                mem::store_bits(
                    &mut self.b,
                    slot.clone(),
                    discriminant,
                    output_bytes,
                    output_register.clone(),
                )?;
                self.b.build_branch(done)?;
                self.b.create_block(no)?;
                mem::store_bits(
                    &mut self.b,
                    slot.clone(),
                    Value::Constant(i64::from(untagged_variant.as_u32())),
                    output_bytes,
                    output_register.clone(),
                )?;
                self.b.build_branch(done)?;
                self.b.create_block(done)?;
                mem::load_bits(&mut self.b, slot, output_bytes, output_register)
            }
        }
    }

    fn resize_tag(&mut self, mut value: Value, from: u64, to: u64, signed: bool) -> Result<Value> {
        let source = layout::integer_carrier(from)?;
        let target = layout::integer_carrier(to)?;
        if from == 1 && signed {
            value = mem::binary(&mut self.b, B::Shl, value, Value::Constant(8), T::I16)?;
            value = mem::binary(&mut self.b, B::AShr, value, Value::Constant(8), T::I16)?;
        }
        if source == target {
            return Ok(value);
        }
        mem::unary(
            &mut self.b,
            if to < from {
                U::Trunc
            } else if signed {
                U::SExt
            } else {
                U::ZExt
            },
            value,
            target,
        )
    }
}
