//! Rust sizes remain octet sizes; one octet occupies one RCC I16 cell.
use rcc_ir::ir::IrType;
use rustc_middle::ty::layout::TyAndLayout;
use rustc_middle::ty::{self, Ty, TyCtxt, TypingEnv};

pub type Result<T> = std::result::Result<T, String>;

pub fn layout<'tcx>(tcx: TyCtxt<'tcx>, ty: Ty<'tcx>) -> Result<TyAndLayout<'tcx>> {
    tcx.layout_of(TypingEnv::fully_monomorphized().as_query_input(ty))
        .map_err(|e| format!("cannot lay out {ty}: {e:?}"))
}

pub fn size<'tcx>(tcx: TyCtxt<'tcx>, ty: Ty<'tcx>) -> Result<u64> {
    Ok(layout(tcx, ty)?.size.bytes())
}

pub fn pointer_type() -> IrType {
    IrType::FatPtr(Box::new(IrType::I16))
}

/// Carrier registers use RCC widths. Object storage always uses Rust's layout,
/// never the carrier's size_in_cells (a Rust u32 occupies FOUR octet-cells).
pub fn carrier<'tcx>(tcx: TyCtxt<'tcx>, ty: Ty<'tcx>) -> Result<Option<IrType>> {
    let width = match ty.kind() {
        ty::Bool => 1,
        ty::Char => 4,
        ty::FnPtr(..) => 4,
        ty::Int(_) | ty::Uint(_) => size(tcx, ty)?,
        ty::RawPtr(..) | ty::Ref(..) if size(tcx, ty)? == 4 => return Ok(Some(pointer_type())),
        ty::Array(..)
        | ty::Tuple(..)
        | ty::Adt(..)
        | ty::Closure(..)
        | ty::RawPtr(..)
        | ty::Ref(..) => {
            return Ok(None);
        }
        ty::Never => return Ok(None),
        _ => return Err(format!("unsupported Rust type: {ty}")),
    };
    Ok(Some(integer_carrier(width)?))
}

pub fn integer_carrier(width: u64) -> Result<IrType> {
    Ok(match width {
        1 | 2 => IrType::I16,
        4 => IrType::I32,
        8 => IrType::I64,
        _ => {
            return Err(format!("unsupported scalar width: {width} Rust bytes"));
        }
    })
}

pub fn pointee(ty: Ty<'_>) -> Result<Ty<'_>> {
    match ty.kind() {
        ty::Ref(_, t, _) | ty::RawPtr(t, _) => Ok(*t),
        ty::Adt(d, args) if d.is_box() => Ok(args.type_at(0)),
        _ => Err(format!("expected pointer, got {ty}")),
    }
}

pub fn signed(ty: Ty<'_>) -> bool {
    matches!(ty.kind(), ty::Int(_))
}

/// Slice metadata also belongs to structs whose final field is a slice (or
/// another such struct), as used by core's array iterator and heapless.
pub fn slice_tail<'tcx>(tcx: TyCtxt<'tcx>, mut ty: Ty<'tcx>) -> Option<Ty<'tcx>> {
    loop {
        match ty.kind() {
            ty::Slice(_) | ty::Str => return Some(ty),
            ty::Adt(def, args) if def.is_struct() => {
                ty = def.non_enum_variant().fields.iter().last()?.ty(tcx, args);
            }
            _ => return None,
        }
    }
}

pub fn dynamic_tail<'tcx>(tcx: TyCtxt<'tcx>, mut ty: Ty<'tcx>) -> Option<Ty<'tcx>> {
    loop {
        match ty.kind() {
            ty::Slice(_) | ty::Str | ty::Dynamic(..) => return Some(ty),
            ty::Adt(def, args) if def.is_struct() => {
                ty = def.non_enum_variant().fields.iter().last()?.ty(tcx, args);
            }
            _ => return None,
        }
    }
}

/// Static prefix and element stride for a slice-tail DST. Its final dynamic
/// size is rounded up to the enclosing type's ABI alignment.
pub fn slice_tail_layout<'tcx>(tcx: TyCtxt<'tcx>, ty: Ty<'tcx>) -> Result<(u64, u64)> {
    match ty.kind() {
        ty::Slice(element) => Ok((0, size(tcx, *element)?)),
        ty::Str => Ok((0, 1)),
        ty::Adt(def, args) if def.is_struct() => {
            let fields = &def.non_enum_variant().fields;
            let (index, field) = fields
                .iter_enumerated()
                .last()
                .ok_or("empty unsized struct")?;
            let (prefix, stride) = slice_tail_layout(tcx, field.ty(tcx, args))?;
            Ok((
                layout(tcx, ty)?.fields.offset(index.index()).bytes() + prefix,
                stride,
            ))
        }
        _ => Err(format!("unsupported dynamic layout for {ty}")),
    }
}
