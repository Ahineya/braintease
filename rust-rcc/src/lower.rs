//! A deliberately bounded typed-MIR backend. Unsupported operations fail closed.
use crate::{
    layout::{self, Result},
    memory as mem,
};
use rcc_ir::{
    BankTag,
    ir::{
        FatPointer, Function, GlobalVariable, IrBinaryOp as B, IrBuilder, IrType as T,
        IrUnaryOp as U, Linkage, Module, Value,
    },
};
use rustc_hir::def_id::DefId;
use rustc_middle::{
    mir::{
        self, ConstValue, Operand, Place, ProjectionElem as P, Rvalue, StatementKind as S,
        TerminatorKind as K,
    },
    ty::{self, Instance, Ty, TyCtxt, TypingEnv},
};
use std::collections::HashMap;

mod calls;
mod checked;
mod constants;
mod enums;
mod runtime;

#[derive(Clone)]
struct Datum<'tcx> {
    value: Value,
    ty: Ty<'tcx>,
    indirect: bool,
}
struct Address<'tcx> {
    pointer: Value,
    ty: Ty<'tcx>,
    length: Option<Value>,
    variant: Option<rustc_abi::VariantIdx>,
}

pub struct Backend<'tcx> {
    tcx: TyCtxt<'tcx>,
    pub module: Module,
    names: HashMap<Instance<'tcx>, String>,
    callers: HashMap<Instance<'tcx>, Instance<'tcx>>,
    queue: Vec<Instance<'tcx>>,
    allocations: HashMap<mir::interpret::AllocId, usize>,
    constant_data: Vec<constants::DataAllocation>,
    function_tokens: Vec<Instance<'tcx>>,
    dispatches: Vec<calls::Signature<'tcx>>,
}

impl<'tcx> Backend<'tcx> {
    pub fn new(tcx: TyCtxt<'tcx>, native: bool) -> Self {
        let mut module = Module::new("rustcell".into());
        if native {
            module.target = rcc_ir::target::Target::Native;
        }
        Self {
            tcx,
            module,
            names: HashMap::new(),
            callers: HashMap::new(),
            queue: Vec::new(),
            allocations: HashMap::new(),
            constant_data: Vec::new(),
            function_tokens: Vec::new(),
            dispatches: Vec::new(),
        }
    }

    fn name(&mut self, instance: Instance<'tcx>, caller: Instance<'tcx>) -> Result<String> {
        if let Some(name) = self.names.get(&instance) {
            return Ok(name.clone());
        }
        if self.queue.len() >= 1024 {
            return Err("prototype exceeds 1024 function instances".into());
        }
        let name = format!("rustcell_fn_{}", self.queue.len());
        self.names.insert(instance, name.clone());
        self.callers.insert(instance, caller);
        self.queue.push(instance);
        Ok(name)
    }

    pub fn compile(mut self, entry: DefId) -> Result<Module> {
        let instance = Instance::mono(self.tcx, entry);
        if std::iter::once(rustc_hir::def_id::LOCAL_CRATE)
            .chain(self.tcx.crates(()).iter().copied())
            .any(|c| self.tcx.has_global_allocator(c))
        {
            return Err("custom global allocators are not supported yet; this prototype uses its guest heap bridge".into());
        }
        self.names.insert(instance, "rustcell_entry".into());
        self.queue.push(instance);
        let mut index = 0;
        loop {
            while index < self.queue.len() {
                let instance = self.queue[index];
                let name = self.names[&instance].clone();
                let mut f = FunctionCx::new(&mut self, instance)?
                    .compile(name.clone())
                    .map_err(|e| {
                        let mut chain = Vec::new();
                        let mut child = instance;
                        while let Some(parent) = self.callers.get(&child) {
                            chain.push(self.tcx.def_path_str(parent.def_id()));
                            child = *parent;
                            if chain.len() == 12 {
                                break;
                            }
                        }
                        format!(
                            "{} ({name}): {e}\nreachable from: {}",
                            self.tcx.def_path_str(instance.def_id()),
                            chain.join(" <- ")
                        )
                    })?;
                f.blocks[0].instructions.insert(
                    0,
                    rcc_ir::ir::Instruction::Comment(format!(
                        "{} {:?}",
                        self.tcx.def_path_str(instance.def_id()),
                        instance.args
                    )),
                );
                self.module.add_function(f);
                index += 1;
            }
            if !self.dispatch_targets()? {
                break;
            }
        }
        self.emit_dispatchers()?;
        self.initialize_constants()?;
        Ok(self.module)
    }

    fn bytes_global(&mut self, bytes: &[u8]) -> Value {
        self.data_global(bytes, true)
    }

    fn data_global(&mut self, bytes: &[u8], is_constant: bool) -> Value {
        let name = format!("rustcell_data_{}", self.module.globals.len());
        self.module.globals.push(GlobalVariable {
            name: name.clone(),
            var_type: T::Array {
                size: bytes.len().max(1) as u64,
                element_type: Box::new(T::I16),
            },
            is_constant,
            initializer: Some(Value::ConstantArray(if bytes.is_empty() {
                vec![0]
            } else {
                bytes.iter().map(|&v| i64::from(v)).collect()
            })),
            linkage: Linkage::Internal,
            symbol_id: None,
        });
        Value::FatPtr(FatPointer {
            addr: Box::new(Value::Global(name)),
            bank: BankTag::Global,
        })
    }
}

struct FunctionCx<'a, 'tcx> {
    backend: &'a mut Backend<'tcx>,
    instance: Instance<'tcx>,
    body: &'tcx mir::Body<'tcx>,
    b: IrBuilder,
    locals: Vec<Value>,
    types: Vec<Ty<'tcx>>,
    labels: Vec<u32>,
    return_pointer: Option<Value>,
}

impl<'a, 'tcx> FunctionCx<'a, 'tcx> {
    fn new(backend: &'a mut Backend<'tcx>, instance: Instance<'tcx>) -> Result<Self> {
        let body = backend.tcx.instance_mir(instance.def);
        let types = body
            .local_decls
            .iter()
            .map(|d| {
                instance.instantiate_mir_and_normalize_erasing_regions(
                    backend.tcx,
                    TypingEnv::fully_monomorphized(),
                    ty::EarlyBinder::bind(d.ty),
                )
            })
            .collect();
        Ok(Self {
            backend,
            instance,
            body,
            b: IrBuilder::new(),
            locals: Vec::new(),
            types,
            labels: Vec::new(),
            return_pointer: None,
        })
    }

    fn tcx(&self) -> TyCtxt<'tcx> {
        self.backend.tcx
    }
    fn mono<T: ty::TypeFoldable<TyCtxt<'tcx>>>(&self, v: T) -> T {
        self.instance.instantiate_mir_and_normalize_erasing_regions(
            self.tcx(),
            TypingEnv::fully_monomorphized(),
            ty::EarlyBinder::bind(v),
        )
    }

    fn compile(mut self, name: String) -> Result<Function> {
        let tcx = self.tcx();
        let ret = self.types[0];
        let return_type = if layout::size(tcx, ret)? == 0 {
            T::Void
        } else {
            layout::carrier(tcx, ret)?.unwrap_or(T::Void)
        };
        self.b.create_function(name.clone(), return_type);
        let mut parameters = Vec::new();
        if layout::size(tcx, ret)? != 0 && layout::carrier(tcx, ret)?.is_none() {
            let id = self.b.new_temp();
            self.b.add_parameter(id, layout::pointer_type());
            self.return_pointer = Some(mem::pointer(Value::Temp(id)));
        }
        for index in 1..=self.body.arg_count {
            let ty = self.types[index];
            let fields: Vec<_> = if self.body.spread_arg.map(|local| local.index()) == Some(index) {
                let ty::Tuple(fields) = ty.kind() else {
                    return Err("RustCall spread argument is not a tuple".into());
                };
                fields
                    .iter()
                    .enumerate()
                    .map(|(i, t)| (Some(i), t))
                    .collect()
            } else {
                vec![(None, ty)]
            };
            for (field, ty) in fields {
                if layout::size(tcx, ty)? == 0 {
                    continue;
                }
                let id = self.b.new_temp();
                let carrier = layout::carrier(tcx, ty)?;
                self.b
                    .add_parameter(id, carrier.clone().unwrap_or_else(layout::pointer_type));
                parameters.push((
                    index,
                    field,
                    Datum {
                        value: Value::Temp(id),
                        ty,
                        indirect: carrier.is_none(),
                    },
                ));
            }
        }
        let entry = self.b.new_label();
        self.b.create_block(entry)?;
        if name == "rustcell_entry" {
            self.b.build_call(
                Value::Function("rustcell_init_constants".into()),
                vec![],
                T::Void,
            )?;
        }
        for ty in &self.types {
            let l = layout::layout(tcx, *ty)?;
            let p = mem::alloca(&mut self.b, l.size.bytes(), l.align.abi.bytes())?;
            self.locals.push(p);
        }
        for (local, field, value) in parameters {
            let mut destination = self.locals[local].clone();
            if let Some(field) = field {
                let offset = layout::layout(tcx, self.types[local])?
                    .fields
                    .offset(field)
                    .bytes();
                destination =
                    mem::offset(&mut self.b, destination, Value::Constant(offset as i64))?;
            }
            self.assign(destination, value.ty, value)?;
        }
        self.labels = self
            .body
            .basic_blocks
            .iter()
            .map(|_| self.b.new_label())
            .collect();
        self.b.build_branch(self.labels[0])?;
        let abort_blocks = self.abort_blocks()?;
        for (bb, data) in self.body.basic_blocks.iter_enumerated() {
            let label = self.labels[bb.index()];
            let loop_header = self.body.basic_blocks.predecessors()[bb]
                .iter()
                .any(|p| p.index() >= bb.index());
            if loop_header {
                self.b.create_loop_condition_block(label)?;
            } else {
                self.b.create_block(label)?;
            }
            // The fixed abort bridge does not format panic arguments. Some
            // core helpers prepare a dyn Debug value immediately before a
            // diverging panic call (notably Result::unwrap). Elide that pure
            // local preparation, without accepting general trait objects or
            // discarding calls, indirect stores, or memory intrinsics.
            if abort_blocks[bb.index()] {
                self.panic()?;
                continue;
            }
            for statement in &data.statements {
                match &statement.kind {
                    S::Assign(assignment) => {
                        let (place, value) = &**assignment;
                        self.rvalue(*place, value)
                            .map_err(|e| format!("{statement:?}: {e}"))?;
                    }
                    S::StorageLive(_) | S::StorageDead(_) | S::Nop => {}
                    S::Intrinsic(intrinsic)
                        if matches!(
                            **intrinsic,
                            mir::NonDivergingIntrinsic::CopyNonOverlapping(_)
                        ) =>
                    {
                        let mir::NonDivergingIntrinsic::CopyNonOverlapping(copy) = &**intrinsic
                        else {
                            unreachable!()
                        };
                        self.copy_elements(&copy.src, &copy.dst, &copy.count)?;
                    }
                    S::Intrinsic(intrinsic)
                        if matches!(**intrinsic, mir::NonDivergingIntrinsic::Assume(_)) =>
                    {
                        // An optimizer hint, with no runtime effect for valid Rust.
                    }
                    S::SetDiscriminant {
                        place,
                        variant_index,
                    } => {
                        let p = self.address(**place)?;
                        self.set_discriminant(p.pointer, p.ty, *variant_index)?;
                    }
                    _ => return Err(format!("unsupported MIR statement: {statement:?}")),
                }
            }
            self.terminator(&data.terminator().kind)
                .map_err(|e| format!("{:?}: {e}", data.terminator().kind))?;
        }
        Ok(self.b.finish_function().unwrap())
    }

    fn address(&mut self, place: Place<'tcx>) -> Result<Address<'tcx>> {
        let mut address = Address {
            pointer: self.locals[place.local.index()].clone(),
            ty: self.types[place.local.index()],
            length: None,
            variant: None,
        };
        for projection in place.projection {
            match projection {
                P::Deref => {
                    let pointer_ty = address.ty;
                    let pointee = layout::pointee(pointer_ty)?;
                    if matches!(pointee.kind(), ty::Adt(..))
                        && layout::dynamic_tail(self.tcx(), pointee)
                            .is_some_and(|t| matches!(t.kind(), ty::Dynamic(..)))
                    {
                        return Err(
                            "struct tails containing trait objects are not supported yet".into(),
                        );
                    }
                    if layout::size(self.tcx(), pointer_ty)? == 8 {
                        if layout::dynamic_tail(self.tcx(), pointee).is_none() {
                            return Err("unsupported wide pointer".into());
                        }
                        let bits = mem::load_bits(&mut self.b, address.pointer.clone(), 4, T::I32)?;
                        let data =
                            mem::unary(&mut self.b, U::IntToPtr, bits, layout::pointer_type())?;
                        let metadata =
                            mem::offset(&mut self.b, address.pointer, Value::Constant(4))?;
                        address.length = Some(mem::load_bits(&mut self.b, metadata, 4, T::I32)?);
                        address.pointer = mem::pointer(data);
                    } else {
                        let bits = mem::load_bits(&mut self.b, address.pointer, 4, T::I32)?;
                        address.pointer = mem::pointer(mem::unary(
                            &mut self.b,
                            U::IntToPtr,
                            bits,
                            layout::pointer_type(),
                        )?);
                    }
                    address.ty = pointee;
                }
                P::Field(index, ty) => {
                    let offset = self
                        .variant_layout(address.ty, address.variant)?
                        .fields
                        .offset(index.index())
                        .bytes();
                    address.pointer =
                        mem::offset(&mut self.b, address.pointer, Value::Constant(offset as i64))?;
                    address.ty = self.mono(ty);
                    // Only an unsized tail inherits the enclosing length.
                    // A sized prefix field (including one with Drop) has a
                    // thin address and must not receive slice metadata.
                    if layout::dynamic_tail(self.tcx(), address.ty).is_none() {
                        address.length = None;
                    }
                    address.variant = None;
                }
                P::Downcast(_, variant) => address.variant = Some(variant),
                P::Index(local) => {
                    let element = address.ty.builtin_index().ok_or("indexing non-array")?;
                    let tcx = self.tcx();
                    let index = mem::load(
                        &mut self.b,
                        tcx,
                        self.locals[local.index()].clone(),
                        self.types[local.index()],
                    )?;
                    let stride = layout::size(tcx, element)?;
                    let offset = mem::binary(
                        &mut self.b,
                        B::Mul,
                        index,
                        Value::Constant(stride as i64),
                        T::I32,
                    )?;
                    address.pointer = mem::offset(&mut self.b, address.pointer, offset)?;
                    address.ty = element;
                    address.length = None;
                }
                P::ConstantIndex {
                    offset, from_end, ..
                } => {
                    let element = address.ty.builtin_index().ok_or("indexing non-array")?;
                    let mut index = Value::Constant(offset as i64);
                    if from_end {
                        let length = self.array_length(address.ty, address.length.clone())?;
                        index = mem::binary(&mut self.b, B::Sub, length, index, T::I32)?;
                    }
                    let stride = layout::size(self.tcx(), element)?;
                    let offset = mem::binary(
                        &mut self.b,
                        B::Mul,
                        index,
                        Value::Constant(stride as i64),
                        T::I32,
                    )?;
                    address.pointer = mem::offset(&mut self.b, address.pointer, offset)?;
                    address.ty = element;
                    address.length = None;
                }
                _ => return Err(format!("unsupported place projection: {projection:?}")),
            }
        }
        Ok(address)
    }

    fn array_length(&self, ty: Ty<'tcx>, metadata: Option<Value>) -> Result<Value> {
        if let ty::Array(_, count) = ty.kind() {
            return Ok(Value::Constant(
                count
                    .try_to_target_usize(self.tcx())
                    .ok_or("non-constant array length")? as i64,
            ));
        }
        metadata.ok_or_else(|| format!("missing length for {ty}"))
    }

    fn unsize_length(&self, source: Ty<'tcx>, target: Ty<'tcx>) -> Result<Value> {
        match (source.kind(), target.kind()) {
            (ty::Array(a, _), ty::Slice(b)) if a == b => self.array_length(source, None),
            (ty::Adt(a, x), ty::Adt(b, y)) if a == b && a.is_struct() => {
                let tail = a
                    .non_enum_variant()
                    .fields
                    .iter()
                    .last()
                    .ok_or("empty unsizing struct")?;
                self.unsize_length(tail.ty(self.tcx(), x), tail.ty(self.tcx(), y))
            }
            _ => Err(format!("unsupported unsizing from {source} to {target}")),
        }
    }

    fn operand(&mut self, operand: &Operand<'tcx>) -> Result<Datum<'tcx>> {
        let tcx = self.tcx();
        match operand {
            Operand::Copy(place) | Operand::Move(place) => {
                let p = self.address(*place)?;
                if layout::size(tcx, p.ty)? == 0 || layout::carrier(tcx, p.ty)?.is_none() {
                    Ok(Datum {
                        value: p.pointer,
                        ty: p.ty,
                        indirect: true,
                    })
                } else {
                    Ok(Datum {
                        value: mem::load(&mut self.b, tcx, p.pointer, p.ty)?,
                        ty: p.ty,
                        indirect: false,
                    })
                }
            }
            Operand::Constant(c) => {
                let c = self.mono(c.const_);
                let ty = c.ty();
                let value = c
                    .eval(tcx, TypingEnv::fully_monomorphized(), rustc_span::DUMMY_SP)
                    .map_err(|e| format!("constant evaluation failed: {e:?}"))?;
                match value {
                    ConstValue::ZeroSized => Ok(Datum {
                        value: Value::Constant(0),
                        ty,
                        indirect: true,
                    }),
                    ConstValue::Scalar(s) => {
                        let width = layout::size(tcx, ty)?;
                        if width > 8 {
                            return Err(format!("unsupported constant: {ty}"));
                        }
                        if s.try_to_scalar_int().is_err() {
                            let pointer = s
                                .to_pointer(&tcx)
                                .report_err()
                                .map_err(|e| format!("constant pointer: {e:?}"))?;
                            let (provenance, offset) = pointer.into_raw_parts();
                            let id = provenance.ok_or("missing constant provenance")?.alloc_id();
                            if let mir::interpret::GlobalAlloc::Function { instance } =
                                tcx.global_alloc(id)
                            {
                                if offset.bytes() != 0 {
                                    return Err(
                                        "function pointer constant requires a Rust fn-pointer type"
                                            .into(),
                                    );
                                }
                                let token = self.backend.function_token(instance)?;
                                if layout::carrier(tcx, ty)?.is_none() {
                                    let l = layout::layout(tcx, ty)?;
                                    let p = mem::alloca(&mut self.b, width, l.align.abi.bytes())?;
                                    mem::store_bits(
                                        &mut self.b,
                                        p.clone(),
                                        Value::Constant(token as i64),
                                        width,
                                        T::I32,
                                    )?;
                                    return Ok(Datum {
                                        value: p,
                                        ty,
                                        indirect: true,
                                    });
                                }
                                if matches!(ty.kind(), ty::RawPtr(..)) {
                                    let value = mem::pointer(mem::unary(
                                        &mut self.b,
                                        U::IntToPtr,
                                        Value::Constant(token as i64),
                                        layout::pointer_type(),
                                    )?);
                                    return Ok(Datum {
                                        value,
                                        ty,
                                        indirect: false,
                                    });
                                }
                                return Ok(Datum {
                                    value: Value::Constant(token as i64),
                                    ty,
                                    indirect: false,
                                });
                            }
                            let value = self.allocation(id, offset.bytes() as usize, None)?;
                            if layout::carrier(tcx, ty)?.is_none() {
                                // A niche-encoded ADT may itself be a pointer.
                                let l = layout::layout(tcx, ty)?;
                                let p = mem::alloca(&mut self.b, width, l.align.abi.bytes())?;
                                let bits = mem::unary(&mut self.b, U::PtrToInt, value, T::I32)?;
                                mem::store_bits(&mut self.b, p.clone(), bits, width, T::I32)?;
                                return Ok(Datum {
                                    value: p,
                                    ty,
                                    indirect: true,
                                });
                            }
                            return Ok(Datum {
                                value,
                                ty,
                                indirect: false,
                            });
                        }
                        let bits = s
                            .try_to_scalar_int()
                            .map_err(|_| "pointer constants with provenance are not supported yet")?
                            .to_bits(rustc_abi::Size::from_bytes(width))
                            as u64;
                        if layout::carrier(tcx, ty)?.is_none() {
                            // rustc represents e.g. None::<&T> and unit variants
                            // as scalar constants even though our private ABI
                            // passes all ADTs as aggregates.
                            let l = layout::layout(tcx, ty)?;
                            let p = mem::alloca(&mut self.b, width, l.align.abi.bytes())?;
                            let register = layout::integer_carrier(width)?;
                            mem::store_bits(
                                &mut self.b,
                                p.clone(),
                                Value::Constant(bits as i64),
                                width,
                                register,
                            )?;
                            return Ok(Datum {
                                value: p,
                                ty,
                                indirect: true,
                            });
                        }
                        let mut v = Value::Constant(bits as i64);
                        if layout::signed(ty) && width < 8 {
                            let shift = 64 - 8 * width;
                            v = Value::Constant(((bits << shift) as i64) >> shift);
                        }
                        if matches!(ty.kind(), ty::RawPtr(..) | ty::Ref(..)) {
                            v = mem::pointer(mem::unary(
                                &mut self.b,
                                U::IntToPtr,
                                v,
                                layout::pointer_type(),
                            )?);
                        }
                        Ok(Datum {
                            value: v,
                            ty,
                            indirect: false,
                        })
                    }
                    ConstValue::Slice { alloc_id, meta } => {
                        let data = self.allocation(alloc_id, 0, None)?;
                        let p = mem::alloca(&mut self.b, 8, 4)?;
                        let bits = mem::unary(&mut self.b, U::PtrToInt, data, T::I32)?;
                        mem::store_bits(&mut self.b, p.clone(), bits, 4, T::I32)?;
                        let m = mem::offset(&mut self.b, p.clone(), Value::Constant(4))?;
                        mem::store_bits(&mut self.b, m, Value::Constant(meta as i64), 4, T::I32)?;
                        Ok(Datum {
                            value: p,
                            ty,
                            indirect: true,
                        })
                    }
                    ConstValue::Indirect { alloc_id, offset } => {
                        let p = self.allocation(
                            alloc_id,
                            offset.bytes() as usize,
                            Some(layout::size(tcx, ty)? as usize),
                        )?;
                        Ok(Datum {
                            value: p,
                            ty,
                            indirect: true,
                        })
                    }
                }
            }
            Operand::RuntimeChecks(check) => Ok(Datum {
                value: Value::Constant(check.value(self.tcx().sess) as i64),
                ty: tcx.types.bool,
                indirect: false,
            }),
        }
    }

    fn allocation(
        &mut self,
        id: mir::interpret::AllocId,
        offset: usize,
        len: Option<usize>,
    ) -> Result<Value> {
        let index = self.backend.register_allocation(id)?;
        let allocation = &self.backend.constant_data[index];
        if offset > allocation.length || len.is_some_and(|len| len > allocation.length - offset) {
            return Err("constant allocation range out of bounds".into());
        }
        let pointer = allocation.pointer(&mut self.b)?;
        mem::offset(&mut self.b, pointer, Value::Constant(offset as i64))
    }

    fn assign(&mut self, destination: Value, ty: Ty<'tcx>, value: Datum<'tcx>) -> Result<()> {
        let tcx = self.tcx();
        let bytes = layout::size(tcx, ty)?;
        if bytes == 0 {
            return Ok(());
        }
        if bytes != layout::size(tcx, value.ty)? {
            return Err("assignment size mismatch".into());
        }
        if value.indirect {
            mem::copy(&mut self.b, destination, value.value, bytes)
        } else {
            mem::store(&mut self.b, tcx, destination, value.value, ty)
        }
    }

    fn scalar(&mut self, operand: &Operand<'tcx>) -> Result<Datum<'tcx>> {
        let d = self.operand(operand)?;
        if d.indirect {
            Err(format!("expected scalar, got {}", d.ty))
        } else {
            Ok(d)
        }
    }

    fn rvalue(&mut self, destination: Place<'tcx>, rvalue: &Rvalue<'tcx>) -> Result<()> {
        let p = self.address(destination)?;
        let tcx = self.tcx();
        match rvalue {
            Rvalue::Use(operand, _) => {
                let v = self.operand(operand)?;
                self.assign(p.pointer, p.ty, v)
            }
            Rvalue::Ref(_, _, source) | Rvalue::RawPtr(_, source) => {
                let source = self.address(*source)?;
                if layout::size(tcx, p.ty)? == 8 {
                    let length = source
                        .length
                        .ok_or("missing slice metadata when taking reference")?;
                    let bits = mem::unary(&mut self.b, U::PtrToInt, source.pointer, T::I32)?;
                    mem::store_bits(&mut self.b, p.pointer.clone(), bits, 4, T::I32)?;
                    let metadata = mem::offset(&mut self.b, p.pointer, Value::Constant(4))?;
                    mem::store_bits(&mut self.b, metadata, length, 4, T::I32)
                } else {
                    mem::store(&mut self.b, tcx, p.pointer, source.pointer, p.ty)
                }
            }
            Rvalue::Aggregate(kind, operands) => {
                let variant = match &**kind {
                    mir::AggregateKind::Adt(_, variant, _, _, _) => Some(*variant),
                    mir::AggregateKind::Array(_)
                    | mir::AggregateKind::Tuple
                    | mir::AggregateKind::RawPtr(..)
                    | mir::AggregateKind::Closure(..) => None,
                    _ => return Err(format!("unsupported aggregate kind: {kind:?}")),
                };
                let l = self.variant_layout(p.ty, variant)?;
                for (index, operand) in operands.iter_enumerated() {
                    let v = self.operand(operand)?;
                    if layout::size(tcx, v.ty)? == 0 {
                        continue;
                    }
                    let field = match &**kind {
                        mir::AggregateKind::Adt(_, _, _, _, Some(field)) => field.index(),
                        _ => index.index(),
                    };
                    let offset = if matches!(&**kind, mir::AggregateKind::RawPtr(..)) {
                        // Thin raw pointers also have a MIR aggregate form:
                        // (data, unit metadata), despite a primitive layout.
                        field as u64 * 4
                    } else {
                        l.fields.offset(field).bytes()
                    };
                    let address = mem::offset(
                        &mut self.b,
                        p.pointer.clone(),
                        Value::Constant(offset as i64),
                    )?;
                    self.assign(address, v.ty, v)?;
                }
                if let Some(variant) = variant {
                    self.set_discriminant(p.pointer, p.ty, variant)?;
                }
                Ok(())
            }
            Rvalue::Discriminant(place) => {
                let source = self.address(*place)?;
                let value = self.read_discriminant(source.pointer, source.ty, p.ty)?;
                mem::store(&mut self.b, tcx, p.pointer, value, p.ty)
            }
            Rvalue::Repeat(operand, count) => {
                let count = self
                    .mono(*count)
                    .try_to_target_usize(tcx)
                    .ok_or("non-constant repeat")?;
                let v = self.operand(operand)?;
                let stride = layout::size(tcx, v.ty)?;
                if count * stride > 1024 {
                    return Err("repeat exceeds prototype limit".into());
                }
                for i in 0..count {
                    let address = mem::offset(
                        &mut self.b,
                        p.pointer.clone(),
                        Value::Constant((i * stride) as i64),
                    )?;
                    self.assign(address, v.ty, v.clone())?;
                }
                Ok(())
            }
            Rvalue::Cast(kind, operand, target) => {
                let target = self.mono(*target);
                let v = self.operand(operand)?;
                if matches!(
                    kind,
                    mir::CastKind::PointerCoercion(
                        ty::adjustment::PointerCoercion::ClosureFnPointer(_),
                        _
                    )
                ) {
                    let ty::Closure(def, args) = v.ty.kind() else {
                        return Err("non-closure function coercion".into());
                    };
                    if layout::size(tcx, v.ty)? != 0 {
                        return Err("capturing closure cannot become a function pointer".into());
                    }
                    let instance =
                        Instance::resolve_closure(tcx, *def, args, ty::ClosureKind::FnOnce);
                    let token = self.backend.function_token(instance)?;
                    return mem::store(
                        &mut self.b,
                        tcx,
                        p.pointer,
                        Value::Constant(token as i64),
                        target,
                    );
                }
                if matches!(
                    kind,
                    mir::CastKind::PointerCoercion(
                        ty::adjustment::PointerCoercion::ReifyFnPointer(_),
                        _
                    )
                ) {
                    let ty::FnDef(def, args) = v.ty.kind() else {
                        return Err("reifying a non-function".into());
                    };
                    let instance = Instance::resolve_for_fn_ptr(
                        tcx,
                        TypingEnv::fully_monomorphized(),
                        *def,
                        args,
                    )
                    .ok_or("unresolved function pointer")?;
                    let token = self.backend.function_token(instance)?;
                    return mem::store(
                        &mut self.b,
                        tcx,
                        p.pointer,
                        Value::Constant(token as i64),
                        target,
                    );
                }
                if let mir::CastKind::PointerCoercion(ty::adjustment::PointerCoercion::Unsize, _) =
                    kind
                {
                    let pointee = layout::pointee(v.ty)?;
                    if !matches!(layout::size(tcx, v.ty)?, 4 | 8) || layout::size(tcx, target)? != 8
                    {
                        return Err("unsupported unsizing pointer representation".into());
                    }
                    let old = if v.indirect {
                        Some(v.value.clone())
                    } else {
                        None
                    };
                    let length = self.unsize_metadata(pointee, layout::pointee(target)?, old)?;
                    let bits = if v.indirect {
                        mem::load_bits(&mut self.b, v.value, 4, T::I32)?
                    } else {
                        mem::unary(&mut self.b, U::PtrToInt, mem::pointer(v.value), T::I32)?
                    };
                    mem::store_bits(&mut self.b, p.pointer.clone(), bits, 4, T::I32)?;
                    let metadata = mem::offset(&mut self.b, p.pointer, Value::Constant(4))?;
                    return mem::store_bits(&mut self.b, metadata, length, 4, T::I32);
                }
                if matches!(kind, mir::CastKind::Transmute) {
                    if layout::size(tcx, target)? != layout::size(tcx, v.ty)? {
                        return Err("transmute size mismatch".into());
                    }
                    return if v.indirect {
                        mem::copy(&mut self.b, p.pointer, v.value, layout::size(tcx, target)?)
                    } else {
                        mem::store(&mut self.b, tcx, p.pointer, v.value, v.ty)
                    };
                }
                if v.indirect {
                    if matches!(kind, mir::CastKind::PtrToPtr)
                        && layout::size(tcx, v.ty)? == 8
                        && layout::size(tcx, target)? == 8
                    {
                        return mem::copy(&mut self.b, p.pointer, v.value, 8);
                    }
                    // A slice/str data-pointer cast discards metadata.
                    if matches!(kind, mir::CastKind::PtrToPtr)
                        && layout::size(tcx, v.ty)? == 8
                        && layout::size(tcx, target)? == 4
                    {
                        let bits = mem::load_bits(&mut self.b, v.value, 4, T::I32)?;
                        let pointer = mem::pointer(mem::unary(
                            &mut self.b,
                            U::IntToPtr,
                            bits,
                            layout::pointer_type(),
                        )?);
                        return mem::store(&mut self.b, tcx, p.pointer, pointer, target);
                    }
                    return Err(format!("unsupported aggregate cast: {kind:?}"));
                }
                let source_carrier = layout::carrier(tcx, v.ty)?.ok_or("non-scalar cast")?;
                let target_carrier =
                    layout::carrier(tcx, target)?.ok_or("non-scalar cast target")?;
                let value = match (
                    matches!(source_carrier, T::FatPtr(_)),
                    matches!(target_carrier, T::FatPtr(_)),
                ) {
                    (true, true) => v.value,
                    (true, false) => mem::unary(
                        &mut self.b,
                        U::PtrToInt,
                        mem::pointer(v.value),
                        target_carrier,
                    )?,
                    (false, true) => mem::pointer(mem::unary(
                        &mut self.b,
                        U::IntToPtr,
                        v.value,
                        target_carrier,
                    )?),
                    (false, false) if source_carrier == target_carrier => v.value,
                    (false, false) => {
                        let extend = layout::size(tcx, target)? > layout::size(tcx, v.ty)?;
                        let op = if !extend {
                            U::Trunc
                        } else if layout::signed(v.ty) {
                            U::SExt
                        } else {
                            U::ZExt
                        };
                        mem::unary(&mut self.b, op, v.value, target_carrier)?
                    }
                };
                mem::store(&mut self.b, tcx, p.pointer, value, target)
            }
            Rvalue::UnaryOp(op, operand) => {
                let value = if matches!(op, mir::UnOp::PtrMetadata) {
                    let v = self.operand(operand)?;
                    if !v.indirect || layout::size(tcx, v.ty)? != 8 {
                        return Err("unsupported pointer metadata".into());
                    }
                    let address = mem::offset(&mut self.b, v.value, Value::Constant(4))?;
                    mem::load_bits(&mut self.b, address, 4, T::I32)?
                } else {
                    let v = self.scalar(operand)?;
                    let ty = layout::carrier(tcx, v.ty)?.unwrap();
                    if v.ty.is_bool() {
                        mem::binary(&mut self.b, B::Xor, v.value, Value::Constant(1), ty)?
                    } else {
                        mem::unary(
                            &mut self.b,
                            match op {
                                mir::UnOp::Not => U::Not,
                                mir::UnOp::Neg => U::Neg,
                                _ => unreachable!(),
                            },
                            v.value,
                            ty,
                        )?
                    }
                };
                mem::store(&mut self.b, tcx, p.pointer, value, p.ty)
            }
            Rvalue::BinaryOp(op, operands) => {
                let x = self.scalar(&operands.0)?;
                let y = self.scalar(&operands.1)?;
                if matches!(
                    op,
                    mir::BinOp::AddWithOverflow
                        | mir::BinOp::SubWithOverflow
                        | mir::BinOp::MulWithOverflow
                ) {
                    return self.checked_binary(*op, x, y, p);
                }
                if matches!(op, mir::BinOp::Offset) {
                    let stride = layout::size(tcx, layout::pointee(x.ty)?)?;
                    let offset = mem::binary(
                        &mut self.b,
                        B::Mul,
                        y.value,
                        Value::Constant(stride as i64),
                        T::I32,
                    )?;
                    let pointer = mem::offset(&mut self.b, x.value, offset)?;
                    return mem::store(&mut self.b, tcx, p.pointer, pointer, p.ty);
                }
                let signed = layout::signed(x.ty);
                let operation = match op {
                    mir::BinOp::Add | mir::BinOp::AddUnchecked => B::Add,
                    mir::BinOp::Sub | mir::BinOp::SubUnchecked => B::Sub,
                    mir::BinOp::Mul | mir::BinOp::MulUnchecked => B::Mul,
                    mir::BinOp::Div => {
                        if signed {
                            B::SDiv
                        } else {
                            B::UDiv
                        }
                    }
                    mir::BinOp::Rem => {
                        if signed {
                            B::SRem
                        } else {
                            B::URem
                        }
                    }
                    mir::BinOp::BitAnd => B::And,
                    mir::BinOp::BitOr => B::Or,
                    mir::BinOp::BitXor => B::Xor,
                    mir::BinOp::Shl | mir::BinOp::ShlUnchecked => B::Shl,
                    mir::BinOp::Shr | mir::BinOp::ShrUnchecked => {
                        if signed {
                            B::AShr
                        } else {
                            B::LShr
                        }
                    }
                    mir::BinOp::Eq => B::Eq,
                    mir::BinOp::Ne => B::Ne,
                    mir::BinOp::Lt => {
                        if signed {
                            B::Slt
                        } else {
                            B::Ult
                        }
                    }
                    mir::BinOp::Le => {
                        if signed {
                            B::Sle
                        } else {
                            B::Ule
                        }
                    }
                    mir::BinOp::Gt => {
                        if signed {
                            B::Sgt
                        } else {
                            B::Ugt
                        }
                    }
                    mir::BinOp::Ge => {
                        if signed {
                            B::Sge
                        } else {
                            B::Uge
                        }
                    }
                    _ => return Err(format!("unsupported binary operation: {op:?}")),
                };
                let ty = layout::carrier(tcx, x.ty)?.unwrap();
                let rhs = if matches!(operation, B::Shl | B::LShr | B::AShr) {
                    let rhs_ty = layout::carrier(tcx, y.ty)?.unwrap();
                    let mask = layout::size(tcx, x.ty)? * 8 - 1;
                    mem::binary(
                        &mut self.b,
                        B::And,
                        y.value,
                        Value::Constant(mask as i64),
                        rhs_ty,
                    )?
                } else {
                    y.value
                };
                let result_ty = if p.ty.is_bool() { T::I16 } else { ty };
                let value = mem::binary(&mut self.b, operation, x.value, rhs, result_ty)?;
                mem::store(&mut self.b, tcx, p.pointer, value, p.ty)
            }
            _ => Err(format!("unsupported MIR rvalue: {rvalue:?}")),
        }
    }

    fn panic(&mut self) -> Result<()> {
        self.b
            .build_call(Value::Function("rustcell_panic".into()), vec![], T::Void)?;
        let dead = self.b.new_label();
        self.b.build_branch(dead)?;
        self.b.create_block(dead)?;
        self.b.build_branch(dead)
    }

    fn is_panic_call(&self, func: &Operand<'tcx>) -> Result<bool> {
        let tcx = self.tcx();
        let ty = self.mono(func.ty(&self.body.local_decls, tcx));
        let ty::FnDef(def_id, generics) = ty.kind() else {
            return Ok(false);
        };
        let Some(instance) =
            Instance::try_resolve(tcx, TypingEnv::fully_monomorphized(), *def_id, generics)
                .map_err(|_| "cannot resolve call")?
        else {
            return Ok(false);
        };
        let path = tcx.def_path_str(instance.def_id());
        let crate_name = tcx.crate_name(instance.def_id().krate);
        Ok(match crate_name.as_str() {
            "core" => {
                path.contains("::panicking::")
                    || path == "core::result::unwrap_failed"
                    || path == "core::slice::copy_from_slice_impl::len_mismatch_fail"
                    || (path.contains("::slice::index::") && path.ends_with("::slice_index_fail"))
            }
            "alloc" => {
                (path.contains("::vec::") && path.ends_with("::assert_failed"))
                    || (tcx.is_foreign_item(instance.def_id())
                        && tcx.item_name(instance.def_id()).as_str()
                            == "__rust_alloc_error_handler")
            }
            _ => false,
        })
    }

    fn abort_blocks(&self) -> Result<Vec<bool>> {
        let mut abort = vec![false; self.body.basic_blocks.len()];
        loop {
            let mut changed = false;
            for (bb, data) in self.body.basic_blocks.iter_enumerated() {
                if abort[bb.index()]
                    || !data.statements.iter().all(|s| match &s.kind {
                        S::Assign(a) => a.0.as_local().is_some(),
                        S::StorageLive(_) | S::StorageDead(_) | S::Nop => true,
                        _ => false,
                    })
                {
                    continue;
                }
                let reaches_abort = match &data.terminator().kind {
                    K::Call { func, target, .. } => {
                        self.is_panic_call(func)?
                            || (target.is_some_and(|bb| abort[bb.index()])
                                && self.is_format_constructor(func))
                    }
                    K::Goto { target } => abort[target.index()],
                    _ => false,
                };
                if reaches_abort {
                    abort[bb.index()] = true;
                    changed = true;
                }
            }
            if !changed {
                return Ok(abort);
            }
        }
    }

    fn is_format_constructor(&self, func: &Operand<'tcx>) -> bool {
        let tcx = self.tcx();
        let ty = self.mono(func.ty(&self.body.local_decls, tcx));
        let ty::FnDef(def, _) = ty.kind() else {
            return false;
        };
        let path = tcx.def_path_str(*def);
        tcx.crate_name(def.krate).as_str() == "core"
            && (path.starts_with("core::fmt::rt::Argument::")
                || path.starts_with("core::fmt::Arguments::"))
            && tcx.item_name(*def).as_str().starts_with("new")
    }

    fn terminator(&mut self, terminator: &K<'tcx>) -> Result<()> {
        match terminator {
            K::Goto { target } => self.b.build_branch(self.labels[target.index()]),
            K::Drop { place, target, .. } => {
                let p = self.address(*place)?;
                if matches!(p.ty.kind(), ty::Dynamic(..)) {
                    self.drop_dynamic(p.pointer, p.length.ok_or("missing trait-object metadata")?)?;
                    return self.b.build_branch(self.labels[target.index()]);
                }
                let drop = Instance::resolve_drop_in_place(self.tcx(), p.ty);
                if !matches!(drop.def, ty::InstanceKind::DropGlue(_, None)) {
                    let pointer = if let Some(length) = p.length {
                        let wide = mem::alloca(&mut self.b, 8, 4)?;
                        let bits = mem::unary(&mut self.b, U::PtrToInt, p.pointer, T::I32)?;
                        mem::store_bits(&mut self.b, wide.clone(), bits, 4, T::I32)?;
                        let metadata = mem::offset(&mut self.b, wide.clone(), Value::Constant(4))?;
                        mem::store_bits(&mut self.b, metadata, length, 4, T::I32)?;
                        wide
                    } else {
                        p.pointer
                    };
                    let name = self.backend.name(drop, self.instance)?;
                    self.b
                        .build_call(Value::Function(name), vec![pointer], T::Void)?;
                }
                self.b.build_branch(self.labels[target.index()])
            }
            K::Return => {
                let ty = self.types[0];
                if let Some(p) = self.return_pointer.clone() {
                    let bytes = layout::size(self.tcx(), ty)?;
                    mem::copy(&mut self.b, p, self.locals[0].clone(), bytes)?;
                    self.b.build_return(None)
                } else if layout::size(self.tcx(), ty)? == 0 {
                    self.b.build_return(None)
                } else {
                    let tcx = self.tcx();
                    let v = mem::load(&mut self.b, tcx, self.locals[0].clone(), ty)?;
                    self.b.build_return(Some(v))
                }
            }
            K::SwitchInt { discr, targets } => {
                let v = self.scalar(discr)?;
                if let Value::Constant(value) = v.value {
                    let bits = layout::size(self.tcx(), v.ty)? * 8;
                    let value = (value as u64) & (u64::MAX >> (64 - bits));
                    let target = targets
                        .iter()
                        .find(|(c, _)| *c == u128::from(value))
                        .map(|(_, t)| t)
                        .unwrap_or_else(|| targets.otherwise());
                    return self.b.build_branch(self.labels[target.index()]);
                }
                for (constant, target) in targets.iter() {
                    let next = self.b.new_label();
                    // MIR switch values use the Rust type's raw bit pattern.
                    // An i8 in our I16 carrier has sign extension above bit 7.
                    let value = if layout::size(self.tcx(), v.ty)? == 1 {
                        mem::binary(
                            &mut self.b,
                            B::And,
                            v.value.clone(),
                            Value::Constant(255),
                            T::I16,
                        )?
                    } else {
                        v.value.clone()
                    };
                    // Operands retain their width, but a branch condition must
                    // be a scalar flag, not an I32/I64 value with an absent high word.
                    let equal = mem::binary(
                        &mut self.b,
                        B::Eq,
                        value,
                        Value::Constant(constant as i64),
                        T::I16,
                    )?;
                    self.b
                        .build_branch_cond(equal, self.labels[target.index()], next)?;
                    self.b.create_block(next)?;
                }
                self.b
                    .build_branch(self.labels[targets.otherwise().index()])
            }
            K::Assert {
                cond,
                expected,
                target,
                ..
            } => {
                let v = self.scalar(cond)?;
                let failed = self.b.new_label();
                let good = self.labels[target.index()];
                self.b.build_branch_cond(
                    v.value,
                    if *expected { good } else { failed },
                    if *expected { failed } else { good },
                )?;
                self.b.create_block(failed)?;
                self.panic()
            }
            K::Call {
                func,
                args,
                destination,
                target,
                ..
            } => {
                let tcx = self.tcx();
                let ty = self.mono(func.ty(&self.body.local_decls, tcx));
                let ty::FnDef(def_id, generics) = ty.kind() else {
                    self.dynamic_call(func, args, *destination, None)?;
                    return if let Some(target) = target {
                        self.b.build_branch(self.labels[target.index()])
                    } else {
                        self.panic()
                    };
                };
                let instance =
                    Instance::try_resolve(tcx, TypingEnv::fully_monomorphized(), *def_id, generics)
                        .map_err(|_| "cannot resolve call")?
                        .ok_or("unresolved generic call")?;
                if self.is_panic_call(func)? {
                    return self.panic();
                }
                if let ty::InstanceKind::Virtual(_, slot) = instance.def {
                    self.dynamic_call(func, args, *destination, Some(slot))?;
                    return if let Some(target) = target {
                        self.b.build_branch(self.labels[target.index()])
                    } else {
                        self.panic()
                    };
                }
                let p = self.address(*destination)?;
                if self.allocator_call(instance, args, *destination)? {
                    // The pinned alloc crate uses the fixed guest allocator ABI.
                } else if let Some(intrinsic) = tcx.intrinsic(instance.def_id()) {
                    if self.memory_intrinsic(intrinsic.name.as_str(), instance, args, &p)? {
                        if let Some(target) = target {
                            return self.b.build_branch(self.labels[target.index()]);
                        }
                        return self.panic();
                    }
                    match intrinsic.name.as_str() {
                        "cold_path" => {} // Branch prediction hint only.
                        "assert_inhabited" => {
                            if layout::layout(tcx, instance.args.type_at(0))?.is_uninhabited() {
                                return self.panic();
                            }
                        }
                        "discriminant_value" => {
                            let source = self.scalar(&args[0].node)?;
                            let value = self.read_discriminant(
                                source.value,
                                instance.args.type_at(0),
                                p.ty,
                            )?;
                            mem::store(&mut self.b, tcx, p.pointer, value, p.ty)?;
                        }
                        "size_of" | "min_align_of" | "align_of" => {
                            let ty = instance.args.type_at(0);
                            let l = layout::layout(tcx, ty)?;
                            let value = if intrinsic.name.as_str() == "size_of" {
                                l.size.bytes()
                            } else {
                                l.align.abi.bytes()
                            };
                            mem::store(
                                &mut self.b,
                                tcx,
                                p.pointer,
                                Value::Constant(value as i64),
                                p.ty,
                            )?;
                        }
                        _ => return Err(format!("unsupported intrinsic: {}", intrinsic.name)),
                    }
                } else {
                    let foreign = tcx.is_foreign_item(instance.def_id());
                    let name = if foreign {
                        let name = tcx.item_name(instance.def_id()).to_string();
                        self.check_bridge(&name, instance)?;
                        name
                    } else {
                        self.backend.name(instance, self.instance)?
                    };
                    let rust_call = tcx
                        .fn_sig(*def_id)
                        .instantiate(tcx, generics)
                        .skip_binder()
                        .abi()
                        == rustc_abi::ExternAbi::RustCall;
                    let values = self.call_values(args, rust_call)?;
                    self.emit_call(Value::Function(name), values, p, None)?;
                }
                if let Some(target) = target {
                    self.b.build_branch(self.labels[target.index()])
                } else {
                    self.panic()
                }
            }
            K::Unreachable => self.panic(),
            _ => Err(format!("unsupported MIR terminator: {terminator:?}")),
        }
    }

    fn check_bridge(&self, name: &str, instance: Instance<'tcx>) -> Result<()> {
        // C16 and Rust have different object layouts. Only these hand-written,
        // signature-checked scalar/pointer thunks are permitted to cross over.
        let signature = self
            .tcx()
            .fn_sig(instance.def_id())
            .instantiate(self.tcx(), instance.args)
            .skip_binder();
        if signature.c_variadic()
            || signature.abi() != (rustc_abi::ExternAbi::C { unwind: false })
            || self
                .tcx()
                .codegen_fn_attrs(instance.def_id())
                .symbol_name
                .is_some()
        {
            return Err(format!(
                "incorrect bridge ABI for {name}: use a non-variadic extern C declaration without link_name"
            ));
        }
        let actual: Vec<_> = signature.inputs().iter().map(|t| t.to_string()).collect();
        let output = signature.output().to_string();
        let (inputs, result): (&[&str], &str) = match name {
            "rustcell_report" => (&["u16"], "()"),
            "rustcell_write" => (&["*const u8", "usize"], "isize"),
            "rustcell_read" => (&["*mut u8", "usize"], "isize"),
            "rustcell_fork" => (&[], "i16"),
            "rustcell_wait" => (&["i16"], "i16"),
            "rustcell_sleep" => (&["u16"], "u16"),
            "rustcell_panic" => (&[], "!"),
            "rustcell_argc" => (&[], "usize"),
            "rustcell_arg" => (&["usize", "*mut u8", "usize"], "isize"),
            "rustcell_open" => (&["*const u8", "usize", "u16"], "isize"),
            "rustcell_fd_read" => (&["i16", "*mut u8", "usize"], "isize"),
            "rustcell_fd_write" => (&["i16", "*const u8", "usize"], "isize"),
            "rustcell_seek" => (&["i16", "i32", "u16"], "isize"),
            "rustcell_close" => (&["i16"], "isize"),
            "rustcell_unlink" => (&["*const u8", "usize"], "isize"),
            _ => {
                return Err(format!(
                    "unapproved C16 FFI symbol: {name}; add an explicit bridge"
                ));
            }
        };
        if actual != inputs || output != result {
            return Err(format!(
                "incorrect bridge signature for {name}: {actual:?} -> {output}"
            ));
        }
        Ok(())
    }
}
