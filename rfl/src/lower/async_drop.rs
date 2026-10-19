//! Pending Async children are destroyed depth first without recursive drivers.
//! Each suspended frame has an unused CALLER link; while being destroyed it
//! links the parent erased owners. Locals/finalizers still run child first.
use super::*;

pub(super) struct AsyncDropWalk {
    current: Value,
    parents: Value,
    head: u32,
}

impl AsyncDropWalk {
    pub(super) fn new(builder: &mut IrBuilder, initial: Value) -> Result<Self, CompileError> {
        let current = builder.build_alloca(ptr_ty(), None).ir()?;
        let parents = builder.build_alloca(ptr_ty(), None).ir()?;
        builder
            .build_store(initial, current.clone(), ptr_ty())
            .ir()?;
        builder
            .build_store(null_ptr(), parents.clone(), ptr_ty())
            .ir()?;
        let head = builder.new_label();
        builder.build_branch(head).ir()?;
        builder.create_block(head).ir()?;
        Ok(Self {
            current,
            parents,
            head,
        })
    }

    pub(super) fn current(&self, builder: &mut IrBuilder) -> Result<Value, CompileError> {
        load_ptr_slot(builder, &self.current)
    }

    pub(super) fn descend(
        &self,
        builder: &mut IrBuilder,
        built: &Built,
        proc: &Proc,
        frame: &Value,
        state: &Value,
    ) -> Result<(), CompileError> {
        let cleanup = builder.new_label();
        for seg in &proc.segs {
            let Some(Term::Suspend { next, awaited, .. }) = &seg.term else {
                continue;
            };
            let Ty::Owned(id) = proc.slots[*awaited].ty else {
                return Err(CompileError::plain(
                    "await cleanup is missing its synthetic owner",
                ));
            };
            let child_ty = built.collections.owners[id as usize];
            // Only compiler-owned Async frames have the private layout below.
            // Custom Future implementations retain their exact cancel protocol.
            if !built.procs.iter().any(|p| p.async_result == Some(child_ty)) {
                continue;
            }
            let yes = builder.new_label();
            let no = builder.new_label();
            let matches = builder
                .build_binary(
                    IrBinaryOp::Eq,
                    state.clone(),
                    Value::Constant(*next as i64),
                    IrType::I1,
                )
                .ir()?;
            builder
                .build_branch_cond(Value::Temp(matches), yes, no)
                .ir()?;
            builder.create_block(yes).ir()?;
            let wrapper = load_fat(builder, frame, proc.slots[*awaited].offset)?;
            Self::require_pointer(builder, &wrapper, cleanup)?;
            let refs = load_cell(builder, &wrapper, OBJ_REFS, IrType::I16)?;
            let unique = builder
                .build_binary(IrBinaryOp::Eq, refs, Value::Constant(1), IrType::I1)
                .ir()?;
            let take = builder.new_label();
            builder
                .build_branch_cond(Value::Temp(unique), take, cleanup)
                .ir()?;
            builder.create_block(take).ir()?;
            let object = load_fat(builder, &wrapper, OWNER_DATA + 1)?;
            Self::require_pointer(builder, &object, cleanup)?;
            let child = load_fat(builder, &object, OBJ_FIELDS)?;
            Self::require_pointer(builder, &child, cleanup)?;
            // This is the same transfer as Async.cancel: clear the shared
            // handle's owner before destroying it. Its later cancel is a no-op.
            let owner = gep(builder, &object, OBJ_FIELDS)?;
            builder.build_store(null_ptr(), owner, ptr_ty()).ir()?;
            let parent = load_ptr_slot(builder, &self.parents)?;
            let link = gep(builder, frame, CALLER)?;
            builder.build_store(parent, link, ptr_ty()).ir()?;
            let current = self.current(builder)?;
            builder
                .build_store(current, self.parents.clone(), ptr_ty())
                .ir()?;
            builder
                .build_store(child, self.current.clone(), ptr_ty())
                .ir()?;
            builder.build_branch(self.head).ir()?;
            builder.create_block(no).ir()?;
        }
        builder.build_branch(cleanup).ir()?;
        builder.create_block(cleanup).ir()?;
        Ok(())
    }

    fn require_pointer(
        builder: &mut IrBuilder,
        pointer: &Value,
        absent: u32,
    ) -> Result<(), CompileError> {
        let live = builder.new_label();
        let missing = ptr_is_null(builder, pointer)?;
        builder.build_branch_cond(missing, absent, live).ir()?;
        builder.create_block(live).ir()?;
        Ok(())
    }

    pub(super) fn finish(&self, builder: &mut IrBuilder) -> Result<(), CompileError> {
        let parent = load_ptr_slot(builder, &self.parents)?;
        let done = builder.new_label();
        let unwind = builder.new_label();
        let empty = ptr_is_null(builder, &parent)?;
        builder.build_branch_cond(empty, done, unwind).ir()?;
        builder.create_block(unwind).ir()?;
        let frame = load_fat(builder, &parent, 2)?;
        let previous = load_fat(builder, &frame, CALLER)?;
        let link = gep(builder, &frame, CALLER)?;
        builder.build_store(null_ptr(), link, ptr_ty()).ir()?;
        builder
            .build_store(previous, self.parents.clone(), ptr_ty())
            .ir()?;
        builder
            .build_store(parent, self.current.clone(), ptr_ty())
            .ir()?;
        builder.build_branch(self.head).ir()?;
        builder.create_block(done).ir()?;
        builder.build_return(None).ir()
    }
}
