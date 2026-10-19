//! Immutable CTFE data, including references to other immutable allocations.
//! The existing C16 globals cannot encode four-octet pointer relocations, so
//! one entry-time initializer aligns backing storage and writes those pointers.
use super::*;
use rustc_abi::Size;
use rustc_middle::mir::interpret::{AllocId, GlobalAlloc, alloc_range};

pub(super) struct DataAllocation {
    source: Value,
    storage: Value,
    slot: Option<Value>,
    pub length: usize,
    alignment: u64,
    relocations: Vec<(u64, Relocation)>,
}

enum Relocation {
    Data(usize, u64),
    Function(u32),
}

impl DataAllocation {
    pub(super) fn pointer(&self, b: &mut IrBuilder) -> Result<Value> {
        match &self.slot {
            Some(slot) => {
                let bits = mem::load_bits(b, slot.clone(), 4, T::I32)?;
                Ok(mem::pointer(mem::unary(
                    b,
                    U::IntToPtr,
                    bits,
                    layout::pointer_type(),
                )?))
            }
            None => Ok(self.storage.clone()),
        }
    }
}

impl<'tcx> Backend<'tcx> {
    pub(super) fn register_allocation(&mut self, id: AllocId) -> Result<usize> {
        if let Some(index) = self.allocations.get(&id) {
            return Ok(*index);
        }
        let allocation = match self.tcx.global_alloc(id) {
            GlobalAlloc::VTable(ty, predicates) => {
                let principal = predicates.principal().map(|p| self.tcx.instantiate_bound_regions_with_erased(p));
                let allocation = self.tcx.vtable_allocation((ty, principal));
                let index = self.register_allocation(allocation)?;
                self.allocations.insert(id, index);
                return Ok(index);
            }
            GlobalAlloc::Memory(allocation) => allocation,
            GlobalAlloc::Static(def) if !self.tcx.is_thread_local_static(def) => self.tcx
                .eval_static_initializer(def).map_err(|_| "cannot evaluate immutable static")?,
            _ => return Err("unsupported constant allocation; mutable/thread-local storage and type-id constants are not supported yet".into()),
        };
        let allocation = allocation.inner();
        if allocation.mutability != rustc_hir::Mutability::Not {
            return Err("mutable constant storage is not supported yet".into());
        }
        let length = allocation.len();
        let alignment = allocation.align.bytes();
        // Packed and linear addresses have the same alignment through 512.
        if alignment > 512 || length > 1_000_000 {
            return Err("constant storage exceeds prototype size/alignment limits".into());
        }
        let bytes = allocation.inspect_with_uninit_and_ptr_outside_interpreter(0..length);
        let source = self.bytes_global(bytes);
        let needs_init = alignment > 1 || !allocation.provenance().ptrs().is_empty();
        let (storage, slot) = if needs_init {
            (
                self.data_global(&vec![0; length.max(1) + alignment as usize - 1], false),
                Some(self.data_global(&[0; 4], false)),
            )
        } else {
            (source.clone(), None)
        };
        let index = self.constant_data.len();
        self.allocations.insert(id, index);
        self.constant_data.push(DataAllocation {
            source,
            storage,
            slot,
            length,
            alignment,
            relocations: Vec::new(),
        });
        // Reserve the node first: immutable data can contain cycles.
        for &(at, _) in allocation.provenance().ptrs().iter() {
            let scalar = allocation
                .read_scalar(&self.tcx, alloc_range(at, Size::from_bytes(4)), true)
                .map_err(|e| format!("constant relocation: {e:?}"))?;
            let pointer = scalar
                .to_pointer(&self.tcx)
                .report_err()
                .map_err(|e| format!("constant pointer: {e:?}"))?;
            let (provenance, offset) = pointer.into_raw_parts();
            let id = provenance.ok_or("missing constant provenance")?.alloc_id();
            let target = match self.tcx.global_alloc(id) {
                GlobalAlloc::Function { instance } => {
                    if offset.bytes() != 0 {
                        return Err("offset function pointer".into());
                    }
                    Relocation::Function(self.function_token(instance)?)
                }
                _ => {
                    let index = self.register_allocation(id)?;
                    if offset.bytes() > self.constant_data[index].length as u64 {
                        return Err("constant pointer is outside its allocation".into());
                    }
                    Relocation::Data(index, offset.bytes())
                }
            };
            self.constant_data[index]
                .relocations
                .push((at.bytes(), target));
        }
        Ok(index)
    }

    pub(super) fn initialize_constants(&mut self) -> Result<()> {
        let mut b = IrBuilder::new();
        b.create_function("rustcell_init_constants".into(), T::Void);
        let entry = b.new_label();
        b.create_block(entry)?;
        // Establish every address before fixing up pointers, including cycles.
        for data in &self.constant_data {
            let Some(slot) = &data.slot else { continue };
            let linear = mem::unary(&mut b, U::PtrToLinear, data.storage.clone(), T::I32)?;
            let negative = mem::unary(&mut b, U::Neg, linear, T::I32)?;
            let padding = mem::binary(
                &mut b,
                B::And,
                negative,
                Value::Constant(data.alignment as i64 - 1),
                T::I32,
            )?;
            let pointer = mem::offset(&mut b, data.storage.clone(), padding)?;
            let bits = mem::unary(&mut b, U::PtrToInt, pointer.clone(), T::I32)?;
            mem::store_bits(&mut b, slot.clone(), bits, 4, T::I32)?;
            let length = mem::binary(
                &mut b,
                B::Or,
                Value::Constant(data.length as i64),
                Value::Constant(0),
                T::I32,
            )?;
            b.build_call(
                Value::Function("rustcell_copy".into()),
                vec![pointer, data.source.clone(), length],
                T::Void,
            )?;
        }
        for data in &self.constant_data {
            for (at, target) in &data.relocations {
                let bits = match *target {
                    Relocation::Function(token) => Value::Constant(token as i64),
                    Relocation::Data(target, offset) => {
                        let target = self.constant_data[target].pointer(&mut b)?;
                        let target = mem::offset(&mut b, target, Value::Constant(offset as i64))?;
                        mem::unary(&mut b, U::PtrToInt, target, T::I32)?
                    }
                };
                let destination = data.pointer(&mut b)?;
                let destination = mem::offset(&mut b, destination, Value::Constant(*at as i64))?;
                mem::store_bits(&mut b, destination, bits, 4, T::I32)?;
            }
        }
        b.build_return(None)?;
        self.module
            .functions
            .push(b.finish_function().ok_or("missing constant initializer")?);
        Ok(())
    }
}
